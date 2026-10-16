"""Video receive state machine for docs/protocol.md (TCP 8081). Device-independent.

The caller supplies `open_socket()` (returns a non-blocking socket whose
connect is already in progress) and the clock, so the same code runs on the
badge, under the emulator with an injected socket, and in host tests.

Socket contract (MicroPython lwIP semantics, py/stream.c + extmod/modlwip.c):
  readinto(buf) -> n > 0 bytes, None when nothing is buffered (EAGAIN), 0 at EOF
  send(buf)     -> bytes written; raises OSError(EAGAIN) while still connecting
"""
import errno

import stream_protocol as P

IDLE = 0  # never connected, or closed by the caller
BACKOFF = 1  # waiting to reconnect
CONNECTING = 2  # TCP connect in progress / HELLO not fully written
HEADER = 3
PAYLOAD = 4
READY = 5  # a complete FRAME sits in the buffer until frame_drawn()

STATE_NAMES = ("idle", "backoff", "connecting", "header", "payload", "ready")

# WHY 1000: the ESP is one hop away. A connect completes in ~15 ms and the ESP
# accepts within ~20 ms (measured), so a SYN still unanswered after 1 s is
# lost, not slow. lwIP only retransmits a SYN after its initial 3 s RTO, so
# giving up and opening a fresh socket is the faster retry. Seen on hardware:
# the first SYN after a Wi-Fi join was lost on 5 of 14 connects in the original
# project; with 3000 here the link watchdog fired first and cycled Wi-Fi.
CONNECT_MS = 1000
# STATUS arrives about once per second whatever the credits, so 3 s of silence
# means the link is dead even if TCP has not noticed yet.
STALL_MS = 3000
BACKOFF_MIN_MS = 250
BACKOFF_MAX_MS = 4000
# Byte counter wraps here so it stays a MicroPython small int (no long-int
# allocation per read after ~1 GiB). Only its changes matter to callers.
BYTES_MASK = 0x3FFFFFFF

_AGAIN = (errno.EAGAIN, getattr(errno, "EWOULDBLOCK", errno.EAGAIN), errno.EINPROGRESS)


class Stream:
    def __init__(self, open_socket, ticks_ms, ticks_diff, framesize=P.FS_QQVGA, quality=6,
                 credits=1, frame_cap=32 * 1024, max_reads=16, spin_ms=0,
                 backoff_max_ms=BACKOFF_MAX_MS, log=None):
        self.open_socket = open_socket
        self.ticks_ms = ticks_ms
        self.ticks_diff = ticks_diff
        self.max_reads = max_reads
        # How long poll() may keep retrying an empty socket once a frame has
        # started arriving. lwIP's 6400 B receive window only reopens when Python
        # reads, so a frame larger than the window would otherwise need one whole
        # display cycle per window.
        self.spin_ms = spin_ms
        self.backoff_max_ms = backoff_max_ms
        self.log = log  # log(msg) on state transitions only, never per frame

        # Every buffer is allocated once, here. The hot path only reads into them.
        self.hello = P.pack_hello(framesize, quality, credits)
        self.hello_mv = memoryview(self.hello)
        self.hdr = bytearray(P.HEADER_SIZE)
        self.hdr_mv = memoryview(self.hdr)
        self.frame = bytearray(frame_cap)
        self.frame_mv = memoryview(self.frame)
        self.status = bytearray(P.STATUS_MAX)
        self.status_mv = memoryview(self.status)
        self.credit = memoryview(P.CREDIT)
        # Slice of the buffer the next read lands in. Rebuilt only after bytes
        # arrive: stream readinto() takes no offset (py/stream.c), and lwIP
        # returns EAGAIN without yielding, so a fresh slice per retry would
        # allocate on every spin iteration.
        self.rx = None

        self.sock = None
        self.state = IDLE
        self.got = 0  # bytes of the current header or payload received
        self.need = 0  # payload length of the current message
        self.mtype = 0
        self.skip = False  # current FRAME is larger than frame_cap: drain and drop it
        self.hello_sent = 0
        self.credits_owed = 0  # credits we could not write yet (EAGAIN)
        self.t_state = ticks_ms()  # connect start or backoff start
        self.t_rx = self.t_state  # last time any byte arrived
        self.t_frame_start = self.t_state
        self.t_open = self.t_state
        self.conn_frames = 0  # frames since the current connect
        # Next wait. Doubles per failure except a connect timeout, resets on a frame.
        self.backoff_ms = BACKOFF_MIN_MS
        self.wait_ms = 0
        self.reason = "start"

        # Latest header values and counters for the overlay.
        self.seq = 0
        self.width = 0
        self.frame_len = 0
        self.age_us = 0
        self.frame_valid = False  # buffer holds the last completed JPEG, safe to redraw
        self.net_ms = 0
        self.frames = 0
        self.statuses = 0
        self.oversize = 0
        self.connects = 0
        self.errors = 0
        self.bytes = 0  # total received, wraps at BYTES_MASK

    # -- public API -------------------------------------------------------

    def poll(self, now):
        """Advance the connection. Returns True when a FRAME is ready to draw."""
        st = self.state
        if st == READY:
            return True
        if st == IDLE or st == BACKOFF:
            if st == BACKOFF and self.ticks_diff(now, self.t_state) < self.wait_ms:
                return False
            self._open(now)
            if self.state != CONNECTING:
                return False
        if self.state == CONNECTING:
            if not self._send_hello(now):
                return False
        if self.credits_owed and not self._send_credits():
            return False
        return self._drain(now)

    def frame_view(self):
        """The completed JPEG, safe to decode while frame_valid is True."""
        return self.frame_mv[:self.frame_len]

    def frame_drawn(self):
        """Call after the frame has been decoded into the framebuffer (or failed to)."""
        if self.state != READY:
            return
        self.state = HEADER
        self.got = 0
        self.rx = None
        self.credits_owed += 1
        self._send_credits()

    def reset_backoff(self):
        """Next reconnect waits the minimum again (call on a Wi-Fi (re)join)."""
        self.backoff_ms = BACKOFF_MIN_MS
        if self.state == BACKOFF:
            self.wait_ms = 0

    def close(self, reason="closed", backoff=True, grow=True):
        """Drop the socket. backoff=False goes IDLE (caller-driven); grow=False
        waits the current backoff without doubling it for the next time."""
        if self.log is not None and self.state >= CONNECTING:
            self.log("tcp close %s after %d frames" % (reason, self.conn_frames))
        sock = self.sock
        self.sock = None
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
        self.reason = reason
        self.got = 0
        self.rx = None
        self.credits_owed = 0
        self.t_state = self.ticks_ms()
        if backoff:
            self.state = BACKOFF
            self.wait_ms = self.backoff_ms
            if grow:
                self.backoff_ms = min(self.backoff_max_ms, self.backoff_ms * 2)
        else:
            self.state = IDLE
            self.backoff_ms = BACKOFF_MIN_MS

    def connected(self):
        return self.state >= HEADER

    # -- internals --------------------------------------------------------

    def _open(self, now):
        self.state = CONNECTING
        self.t_state = now
        self.t_rx = now
        self.hello_sent = 0
        self.credits_owed = 0
        self.got = 0
        self.rx = None
        self.connects += 1
        self.t_open = now
        self.conn_frames = 0
        if self.log is not None:
            self.log("tcp connect #%d" % self.connects)
        try:
            self.sock = self.open_socket()
        except OSError as e:
            self._fail("open %s" % _errno(e))

    def _fail(self, reason, grow=True):
        self.errors += 1
        self.close(reason, grow=grow)

    def _send_hello(self, now):
        try:
            n = self.sock.send(self.hello_mv[self.hello_sent:])
        except OSError as e:
            if _errno(e) in _AGAIN:
                n = 0
            else:
                self._fail("connect %s" % _errno(e))
                return False
        self.hello_sent += n or 0
        if self.hello_sent >= P.HELLO_SIZE:
            self.state = HEADER
            self.got = 0
            self.t_rx = now
            if self.log is not None:
                self.log("tcp up in %d ms" % self.ticks_diff(now, self.t_open))
            return True
        if self.ticks_diff(now, self.t_state) >= CONNECT_MS:
            # WHY grow=False: the doubling backoff exists so a connect the ESP
            # refuses within milliseconds (server not up yet) cannot spin at
            # loop rate. A timed-out connect already waited CONNECT_MS, and its
            # likeliest cause is a lost SYN that a fresh socket fixes, so the
            # retry keeps the current wait: CONNECT_MS + BACKOFF_MIN_MS after a
            # join. If the ESP is really gone, link.py cycles Wi-Fi after
            # NEVER_RX_DEAD_MS of such attempts.
            self._fail("connect timeout", grow=False)
        return False

    def _send_credits(self):
        while self.credits_owed:
            try:
                n = self.sock.send(self.credit)
            except OSError as e:
                if _errno(e) in _AGAIN:
                    return True  # retried on the next poll
                self._fail("credit %s" % _errno(e))
                return False
            if not n:
                return True
            self.credits_owed -= 1
        return True

    def _drain(self, now):
        reads = 0
        spin_until = None
        while reads < self.max_reads:
            # Finishing a message can send a credit, and a failed send closes
            # the socket: stop rather than read from None.
            if self.state < HEADER:
                return False
            reads += 1
            try:
                n = self._readinto()
            except OSError as e:
                if _errno(e) in _AGAIN:
                    n = None
                else:
                    self._fail("recv %s" % _errno(e))
                    return False
            if n is None:
                # Nothing buffered. Keep trying briefly only once a message is
                # partly here; otherwise give the rest of the update back.
                mid = self.state == PAYLOAD or self.got
                if not mid or self.spin_ms <= 0:
                    break
                t = self.ticks_ms()
                if spin_until is None:
                    spin_until = t
                if self.ticks_diff(t, spin_until) >= self.spin_ms:
                    break
                reads -= 1  # spinning on EAGAIN is bounded by spin_ms, not max_reads
                continue
            if n == 0:
                self._fail("eof")
                return False
            self.rx = None
            self.bytes = (self.bytes + n) & BYTES_MASK
            self.t_rx = now
            if self.state == HEADER:
                if self.got == 0:
                    self.t_frame_start = self.ticks_ms()
                self.got += n
                if self.got < P.HEADER_SIZE:
                    continue
                if not self._on_header():
                    return False
                if self.need == 0:
                    if self._on_payload():
                        return True
                continue
            self.got += n
            if self.skip and self.got < self.need:
                continue
            if self.got >= self.need:
                if self._on_payload():
                    return True
        if self.state >= HEADER and self.state != READY and self.ticks_diff(now, self.t_rx) >= STALL_MS:
            self._fail("stalled")
        return False

    def _readinto(self):
        rx = self.rx
        if rx is None:
            if self.state == HEADER:
                rx = self.hdr_mv[self.got:]
            elif self.skip:
                # Oversized frame: drain it through the frame buffer, a buffer at a time.
                left = self.need - self.got
                rx = self.frame_mv[:left if left < len(self.frame) else len(self.frame)]
            elif self.mtype == P.TYPE_STATUS:
                rx = self.status_mv[self.got:self.need]
            else:
                rx = self.frame_mv[self.got:self.need]
            self.rx = rx
        return self.sock.readinto(rx)

    def _on_header(self):
        try:
            mtype, seq, width, length, age = P.unpack_header(self.hdr)
        except P.ProtocolError as e:
            self._fail("bad header: %s" % e.args[0])
            return False
        self.mtype = mtype
        self.need = length
        self.got = 0
        self.skip = mtype == P.TYPE_FRAME and length > len(self.frame)
        if mtype == P.TYPE_FRAME:
            self.frame_valid = False
            self.seq = seq
            self.width = width
            self.age_us = age
        self.state = PAYLOAD
        return True

    def _on_payload(self):
        """Finish the current message. Returns True if a drawable FRAME is ready."""
        self.state = HEADER
        got = self.got
        self.got = 0
        if self.mtype == P.TYPE_STATUS:
            self.statuses += 1
            return False
        if self.skip:
            # Too large to keep: hand the credit back so the ESP keeps sending.
            self.skip = False
            self.oversize += 1
            self.credits_owed += 1
            self._send_credits()
            return False
        self.frame_len = got
        self.frame_valid = True
        self.frames += 1
        t = self.ticks_ms()
        self.net_ms = self.ticks_diff(t, self.t_frame_start)
        if self.conn_frames == 0 and self.log is not None:
            self.log("first frame %d B, %d ms after connect" % (got, self.ticks_diff(t, self.t_open)))
        self.conn_frames += 1
        self.backoff_ms = BACKOFF_MIN_MS
        self.reason = "ok"
        self.state = READY
        return True


def _errno(e):
    return e.errno if getattr(e, "errno", None) is not None else (e.args[0] if e.args else -1)
