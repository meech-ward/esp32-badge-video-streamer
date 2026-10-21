"""Host tests for tufty/video_streamer/stream.py with a scripted fake socket."""
import errno
import json

import pytest

import stream as S  # noqa: E402
import stream_protocol as P  # noqa: E402

AGAIN = object()  # readinto returns None once
EOF = b""


class Clock:
    def __init__(self):
        self.t = 1000

    def ticks_ms(self):
        return self.t

    @staticmethod
    def ticks_diff(a, b):
        return a - b


class ScriptSock:
    """readinto() plays a script of byte chunks, AGAIN, EOF or exceptions."""

    def __init__(self, script=(), send_again=0):
        self.script = list(script)
        self.sent = bytearray()
        self.send_calls = 0
        self.send_again = send_again
        self.closed = False
        self.targets = []  # underlying objects readinto wrote into
        self.read_calls = 0

    def feed(self, *items):
        self.script.extend(items)

    def send(self, buf):
        self.send_calls += 1
        if self.send_again:
            self.send_again -= 1
            raise OSError(errno.EAGAIN)
        self.sent.extend(bytes(buf))
        return len(buf)

    def readinto(self, buf):
        self.read_calls += 1
        self.targets.append(buf.obj)
        if not self.script:
            return None
        item = self.script.pop(0)
        if item is AGAIN:
            return None
        if isinstance(item, Exception):
            raise item
        if item == EOF:
            return 0
        n = min(len(item), len(buf))
        buf[:n] = item[:n]
        if n < len(item):
            self.script.insert(0, item[n:])
        return n

    def close(self):
        self.closed = True


def frame_msg(jpeg, seq=1, width=160, age=5000):
    return P.pack_header(P.TYPE_FRAME, seq, width, len(jpeg), age) + jpeg


def status_msg(obj, seq=1):
    body = json.dumps(obj).encode()
    return P.pack_header(P.TYPE_STATUS, seq, 0, len(body)) + body


def make(socks, **kw):
    clock = Clock()
    it = iter(socks)
    st = S.Stream(lambda: next(it), clock.ticks_ms, clock.ticks_diff, **kw)
    return st, clock


def split(data, *cuts):
    out, last = [], 0
    for c in cuts:
        out.append(data[last:c])
        last = c
    out.append(data[last:])
    return out


JPEG = bytes(range(256)) * 20  # 5120 opaque payload bytes; stream never parses JPEG


def test_hello_waits_for_connect_then_frame_in_partial_reads():
    msg = frame_msg(JPEG, seq=7)
    a, b, c, d = split(msg, 10, P.HEADER_SIZE, 2000)
    sock = ScriptSock([a, AGAIN, b, c, AGAIN, d], send_again=2)
    st, clock = make([sock])
    assert st.poll(clock.t) is False and st.state == S.CONNECTING
    assert st.poll(clock.t) is False and sock.sent == b""
    clock.t += 5
    # Third send succeeds: HELLO out, then one drain pass reads up to the first AGAIN.
    assert st.poll(clock.t) is False
    assert bytes(sock.sent) == P.pack_hello(P.FS_QQVGA, 6, 1, 32)
    assert st.state == S.HEADER and st.got == 10  # EAGAIN mid-header
    assert st.poll(clock.t) is False and st.state == S.PAYLOAD  # EAGAIN mid-payload
    assert st.got == 2000 - P.HEADER_SIZE
    assert st.poll(clock.t) is True
    assert bytes(st.frame_view()) == JPEG
    assert (st.seq, st.width, st.age_us) == (7, 160, 5000)
    assert sock.sent.count(P.CREDIT) == 0  # credit only after draw
    assert st.poll(clock.t) is True  # still READY until drawn, no extra reads
    st.frame_drawn()
    assert bytes(sock.sent[P.HELLO_SIZE:]) == P.CREDIT
    st.frame_drawn()  # a second call for the same frame sends nothing
    assert bytes(sock.sent[P.HELLO_SIZE:]) == P.CREDIT


def test_credit_once_per_drawn_frame_and_buffers_reused():
    sock = ScriptSock(send_again=0)
    st, clock = make([sock])
    hdr_id, frame_id = id(st.hdr), id(st.frame)
    for i in range(5):
        sock.feed(frame_msg(JPEG[: 1000 + i], seq=i))
        while not st.poll(clock.t):
            pass
        assert len(st.frame_view()) == 1000 + i
        st.frame_drawn()
    assert bytes(sock.sent[P.HELLO_SIZE:]) == P.CREDIT * 5
    assert st.frames == 5
    assert id(st.hdr) == hdr_id and id(st.frame) == frame_id
    # Every read landed in one of the three preallocated buffers.
    assert {id(t) for t in sock.targets} <= {id(st.hdr), id(st.frame), id(st.status)}


def test_cached_jpeg_is_valid_only_until_next_frame_starts_overwriting_it():
    sock = ScriptSock([frame_msg(b"first"), AGAIN])
    st, clock = make([sock])
    assert not st.frame_valid
    assert st.poll(clock.t) and st.frame_valid
    st.frame_drawn()
    assert st.frame_valid  # a local HUD redraw can still decode it without a credit
    sock.feed(status_msg({"fps": 25}), AGAIN)
    st.poll(clock.t)
    st.poll(clock.t)
    assert st.frame_valid  # STATUS has a separate buffer
    sock.feed(P.pack_header(P.TYPE_FRAME, 2, 160, 6) + b"sec", AGAIN)
    st.poll(clock.t)
    assert not st.frame_valid
    sock.feed(b"ond")
    assert st.poll(clock.t) and st.frame_valid and bytes(st.frame_view()) == b"second"


def test_status_interleaved_costs_no_credit():
    s1 = status_msg({"fps": 24})
    s2 = status_msg({"fps": 25}, seq=2)
    f = frame_msg(JPEG, seq=1)
    data = s1 + f + s2
    sock = ScriptSock(split(data, 5, len(s1) + 30, len(s1) + 3000, len(s1) + len(f) + 3))
    st, clock = make([sock])
    got = []
    for _ in range(10):
        if st.poll(clock.t):
            got.append(bytes(st.frame_view()))
            st.frame_drawn()
    assert got == [JPEG]
    assert st.statuses == 2
    assert bytes(sock.sent[P.HELLO_SIZE:]) == P.CREDIT


def test_corrupt_magic_reconnects_with_backoff_and_fresh_hello():
    bad = bytearray(frame_msg(JPEG))
    bad[0:2] = b"ZZ"
    s1 = ScriptSock([bytes(bad)])
    s2 = ScriptSock([frame_msg(JPEG)])
    st, clock = make([s1, s2])
    st.poll(clock.t)
    assert st.state == S.BACKOFF and s1.closed and st.reason.startswith("bad header")
    assert st.errors == 1
    assert st.wait_ms == S.BACKOFF_MIN_MS
    clock.t += st.wait_ms - 1
    assert st.poll(clock.t) is False and st.state == S.BACKOFF
    clock.t += 1
    assert st.poll(clock.t) is True
    assert bytes(s2.sent[:P.HELLO_SIZE]) == P.pack_hello(P.FS_QQVGA, 6, 1, 32)
    assert st.connects == 2 and st.backoff_ms == S.BACKOFF_MIN_MS


def test_payload_len_over_protocol_max_is_rejected():
    hdr = bytearray(P.pack_header(P.TYPE_FRAME, 1, 160, 10))
    hdr[8:12] = (P.MAX_PAYLOAD + 1).to_bytes(4, "little")
    sock = ScriptSock([bytes(hdr)])
    st, clock = make([sock, ScriptSock()])
    assert st.poll(clock.t) is False
    assert st.state == S.BACKOFF and st.reason == "bad header: header len" and sock.closed


def test_frame_over_local_cap_is_drained_and_credit_returned():
    big = bytes(5000)
    sock = ScriptSock([frame_msg(big, seq=1), AGAIN, frame_msg(JPEG[:900], seq=2)])
    st, clock = make([sock], frame_cap=1024)
    assert st.poll(clock.t) is False
    assert st.oversize == 1 and st.state == S.HEADER
    assert bytes(sock.sent[P.HELLO_SIZE:]) == P.CREDIT  # handed back undrawn
    assert st.poll(clock.t) is True and bytes(st.frame_view()) == JPEG[:900]
    assert st.seq == 2



def test_oversize_credit_send_failure_backs_off_instead_of_crashing():
    class ResetOnCredit(ScriptSock):
        def send(self, buf):
            if bytes(buf) == P.CREDIT:
                raise OSError(errno.ECONNRESET)
            return ScriptSock.send(self, buf)
    sock = ResetOnCredit([frame_msg(bytes(5000), seq=1), frame_msg(JPEG[:900], seq=2)])
    st, clock = make([sock, ScriptSock()], frame_cap=1024)
    assert st.poll(clock.t) is False
    assert st.state == S.BACKOFF and st.sock is None and sock.closed
    assert st.oversize == 1 and st.errors == 1 and st.reason.startswith("credit")


def test_eagain_spin_reuses_one_receive_slice():
    class Tick(ScriptSock):
        def readinto(self, buf):
            clock.t += 1  # each read costs 1 ms of wall time
            return super().readinto(buf)
    sock = Tick([P.pack_header(P.TYPE_FRAME, 1, 160, 3000) + bytes(100)])
    st, clock = make([sock], spin_ms=8, max_reads=4)
    st.poll(clock.t)  # header + partial payload, then spins until spin_ms
    assert st.state == S.PAYLOAD and sock.read_calls >= 6
    spun = sock.targets[-6:]
    assert all(t is spun[0] for t in spun)  # same slice object every EAGAIN retry
    sock.feed(bytes(2900))
    assert st.poll(clock.t) is True and st.frame_len == 3000
def test_eof_and_reset_back_off_exponentially():
    socks = [ScriptSock([EOF]), ScriptSock([OSError(errno.ECONNRESET)]), ScriptSock([EOF])]
    st, clock = make(socks + [ScriptSock()])
    waits = []
    for _ in range(3):
        while st.state != S.BACKOFF:
            st.poll(clock.t)
        waits.append(st.wait_ms)
        clock.t += st.wait_ms
        st.poll(clock.t)
    assert waits == [250, 500, 1000]
    assert all(s.closed for s in socks)


def test_open_failure_backs_off():
    def boom():
        raise OSError(errno.EHOSTUNREACH)
    clock = Clock()
    st = S.Stream(boom, clock.ticks_ms, clock.ticks_diff)
    assert st.poll(clock.t) is False
    assert st.state == S.BACKOFF and st.reason == "open %d" % errno.EHOSTUNREACH


def test_connect_timeout_retries_with_a_fresh_socket_after_the_minimum_backoff():
    """Lost first SYN: the socket never becomes writable. Seen on hardware (5 of 14 connects)."""
    hung = ScriptSock(send_again=10**6)
    ok = ScriptSock([status_msg({"fps": 0})])
    st, clock = make([hung, ok])
    t0 = clock.t
    st.poll(clock.t)
    clock.t += S.CONNECT_MS - 1
    assert st.poll(clock.t) is False and st.state == S.CONNECTING and st.connects == 1
    clock.t += 1
    st.poll(clock.t)
    assert st.state == S.BACKOFF and st.reason == "connect timeout" and hung.closed
    assert st.errors == 1
    # The wait is the minimum and the next one is not doubled: a timeout already
    # cost CONNECT_MS, unlike a refused connect that returns at once.
    assert st.wait_ms == S.BACKOFF_MIN_MS and st.backoff_ms == S.BACKOFF_MIN_MS
    clock.t += S.BACKOFF_MIN_MS - 1
    assert st.poll(clock.t) is False and st.connects == 1
    clock.t += 1
    st.poll(clock.t)
    assert st.connects == 2 and st.connected() and st.statuses == 1
    assert bytes(ok.sent[:P.HELLO_SIZE]) == P.pack_hello(P.FS_QQVGA, 6, 1, 32)
    assert clock.t - t0 == S.CONNECT_MS + S.BACKOFF_MIN_MS  # ~1.25 s, not a Wi-Fi cycle


def test_repeated_connect_timeouts_keep_the_minimum_wait():
    """ESP gone: every SYN hangs. The retry period stays CONNECT_MS + BACKOFF_MIN_MS."""
    socks = [ScriptSock(send_again=10**6) for _ in range(4)]
    st, clock = make(socks)
    opened = []
    for _ in range(4):
        st.poll(clock.t)
        opened.append(clock.t)
        while st.state != S.BACKOFF:
            clock.t += 10
            st.poll(clock.t)
        assert st.reason == "connect timeout" and st.wait_ms == S.BACKOFF_MIN_MS
        clock.t += st.wait_ms
    assert [b - a for a, b in zip(opened, opened[1:])] == [S.CONNECT_MS + S.BACKOFF_MIN_MS] * 3
    assert st.connects == 4 and st.errors == 4 and st.backoff_ms == S.BACKOFF_MIN_MS
    assert all(s.closed for s in socks)


def test_refused_connect_still_doubles_the_backoff():
    """The ESP's server is not up yet: lwIP reports the RST on send, within milliseconds."""
    class Refused(ScriptSock):
        def send(self, buf):
            raise OSError(errno.ECONNRESET)
    socks = [Refused() for _ in range(3)]
    st, clock = make(socks + [ScriptSock()])
    waits = []
    for _ in range(3):
        st.poll(clock.t)
        assert st.state == S.BACKOFF and st.reason == "connect %d" % errno.ECONNRESET
        waits.append(st.wait_ms)
        clock.t += st.wait_ms
    assert waits == [250, 500, 1000]


def test_connect_ms_is_well_above_a_same_hop_connect():
    # Measured: TCP up in 15-23 ms, the ESP accepts within ~20 ms. lwIP's own
    # SYN retransmit is 3 s, so the timeout must sit well under that to help.
    assert 500 <= S.CONNECT_MS <= 1500


def test_silent_link_is_declared_stalled():
    sock = ScriptSock()
    st, clock = make([sock])
    st.poll(clock.t)
    assert st.state == S.HEADER
    clock.t += S.STALL_MS - 1
    st.poll(clock.t)
    assert st.state == S.HEADER
    clock.t += 1
    st.poll(clock.t)
    assert st.state == S.BACKOFF and st.reason == "stalled"


def test_credit_retried_after_eagain():
    sock = ScriptSock([frame_msg(JPEG)])
    st, clock = make([sock])
    assert st.poll(clock.t) is True
    sock.send_again = 1
    st.frame_drawn()
    assert st.credits_owed == 1 and bytes(sock.sent[P.HELLO_SIZE:]) == b""
    st.poll(clock.t)
    assert st.credits_owed == 0 and bytes(sock.sent[P.HELLO_SIZE:]) == P.CREDIT


def test_reads_per_poll_are_bounded():
    msg = frame_msg(JPEG)
    sock = ScriptSock([msg[i:i + 1] for i in range(len(msg))])
    st, clock = make([sock], max_reads=8)
    st.poll(clock.t)  # HELLO + 8 one-byte reads
    assert sock.read_calls == 8 and st.got == 8
    polls = 1
    while not st.poll(clock.t):
        polls += 1
    assert polls + 1 == -(-len(msg) // 8)  # the last poll completes the frame


def test_spin_waits_for_rest_of_frame_within_budget():
    msg = frame_msg(JPEG)

    class Tick(ScriptSock):
        def readinto(self, buf):
            clock.t += 1  # each read costs 1 ms of wall time
            return super().readinto(buf)

    sock = Tick([msg[:3000], AGAIN, AGAIN, AGAIN, msg[3000:]])
    clock = Clock()
    st = S.Stream(lambda: sock, clock.ticks_ms, clock.ticks_diff, spin_ms=10)
    assert st.poll(clock.t) is True  # rode out three EAGAINs inside one update
    sock2 = Tick([msg[:3000]] + [AGAIN] * 50)
    clock2 = clock
    st2 = S.Stream(lambda: sock2, clock2.ticks_ms, clock2.ticks_diff, spin_ms=10)
    assert st2.poll(clock2.t) is False
    assert sock2.read_calls <= 13  # bounded by the 10 ms budget


def test_idle_header_wait_does_not_spin():
    sock = ScriptSock([AGAIN] * 5)
    st, clock = make([sock], spin_ms=50)
    st.poll(clock.t)
    assert sock.read_calls == 1


def test_net_ms_measured_from_first_header_byte():
    msg = frame_msg(JPEG)
    sock = ScriptSock([msg[:5], AGAIN, msg[5:]])
    st, clock = make([sock])
    st.poll(clock.t)
    clock.t += 17
    assert st.poll(clock.t) is True
    assert st.net_ms == 17


@pytest.mark.parametrize("credits", [1, 2])
def test_hello_carries_settings(credits):
    sock = ScriptSock()
    st, clock = make([sock], framesize=P.FS_QVGA, quality=20, credits=credits)
    st.poll(clock.t)
    assert P.unpack_hello(bytes(sock.sent[:8])) == (P.FS_QVGA, 20, credits)
