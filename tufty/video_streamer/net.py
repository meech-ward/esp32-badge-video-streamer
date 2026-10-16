"""Device glue: join the ESP's SoftAP and open the video socket.

Only imported on the badge (needs `network` and `socket`). Everything here is
non-blocking so HOME keeps working while Wi-Fi joins or the ESP is away.
"""
import errno
import network
import socket

import stream_protocol as P

# CYW43 power management off. Power save parks the radio between beacons and
# adds tens of milliseconds of receive latency. WLAN.PM_NONE is 0x10
# (extmod/network_cyw43.c:50); a build without it is left at its default.
PM_NONE = getattr(network.WLAN, "PM_NONE", None)
JOIN_TIMEOUT_MS = 15000
RETRY_MS = 2000
# status() value for "associated with an IP" (CYW43_LINK_UP, cyw43.h). For the
# STA interface isconnected() is exactly status() == LINK_UP: both read lwIP's
# netif flags (network_cyw43.c, cyw43_tcpip_link_status), so neither notices an
# AP that vanished without a deauth. Liveness is judged by link.Link instead.
LINK_UP = getattr(network, "STAT_GOT_IP", 3)

_STATUS_TEXT = {
    getattr(network, "STAT_WRONG_PASSWORD", -3): "wrong password",
    getattr(network, "STAT_NO_AP_FOUND", -2): "ESP hotspot not found",
    getattr(network, "STAT_CONNECT_FAIL", -1): "join failed",
}


class Wifi:
    def __init__(self, ssid, password, ticks_diff, log=None):
        self.ssid = ssid
        self.password = password
        self.ticks_diff = ticks_diff
        self.wlan = network.WLAN(network.STA_IF)
        self.joining = False
        self.up = False
        self.t = None
        # The launcher may have left the badge on home Wi-Fi; isconnected() would
        # then report a link that cannot reach 192.168.4.1. Always start clean.
        self.fresh = True
        self.text = "joining " + ssid
        self.ip = None
        self.log = log
        self.resets = 0  # forced resets (stale association)
        self.cycles = 0  # of those, how many needed active(False)/active(True)

    def _pm_off(self):
        if PM_NONE is None:
            return
        try:
            self.wlan.config(pm=PM_NONE)
        except (OSError, ValueError, TypeError, AttributeError):
            pass

    def poll(self, now):
        """True while associated with an IP."""
        w = self.wlan
        if self.fresh:
            self.fresh = False
            try:
                if w.active() and w.isconnected():
                    w.disconnect()
            except OSError:
                pass
            return self._join(now)
        if w.isconnected():
            if not self.up:
                self.up = True
                self.joining = False
                # Applied again after association: whether the driver restores
                # its default PM on join is unverified on this firmware.
                self._pm_off()
                self.ip = w.ifconfig()[0]
                self.text = "wifi up " + self.ip
                self._log("wifi up %s" % self.ip)
            return True
        if self.up:
            self.up = False
            self.text = "wifi lost; rejoining"
            self._log("wifi lost")
            self.joining = False
            self.t = None
        if self.joining:
            st = w.status()
            if st in _STATUS_TEXT:
                self.text = _STATUS_TEXT[st]
                self._log("wifi join failed: " + self.text)
                self.joining = False
                self.t = now
            elif self.ticks_diff(now, self.t) >= JOIN_TIMEOUT_MS:
                self.text = "join timeout"
                self._log("wifi join failed: " + self.text)
                try:
                    w.disconnect()
                except OSError:
                    pass
                self.joining = False
                self.t = now
            return False
        if self.t is not None and self.ticks_diff(now, self.t) < RETRY_MS:
            return False
        return self._join(now)

    def _join(self, now):
        w = self.wlan
        try:
            if not w.active():
                w.active(True)
            self._pm_off()
            w.connect(self.ssid, self.password)
            self.joining = True
            self.t = now
            self.text = "joining " + self.ssid
            self._log("wifi join " + self.ssid)
        except OSError as e:
            self.text = "wifi error %s" % (e.args[0] if e.args else "")
            self._log(self.text)
            self.t = now
        return False

    def reset(self, now, reason):
        """Drop a link that claims to be up but carries nothing; poll() rejoins.

        disconnect() is an SET_DISASSOC ioctl; the link only goes down when the
        chip reports EV_DISASSOC/EV_LINK, which may not have happened by the
        time it returns (cyw43_ctrl.c). If status() still says LINK_UP, cycling
        the interface removes the lwIP netif and clears its address, which is
        the only path that is sure to make isconnected() False. active(True)
        keeps the chip powered (cyw43_ensure_up) but reruns wifi_on and resets
        power management, so it blocks briefly and _join turns PM off again.
        """
        self.resets += 1
        w = self.wlan
        try:
            w.disconnect()
        except OSError:
            pass
        cycled = False
        try:
            if w.status() == LINK_UP:
                w.active(False)
                w.active(True)
                cycled = True
                self.cycles += 1
        except OSError:
            pass
        self._log("wifi reset: %s%s" % (reason, " (cycled)" if cycled else ""))
        self.up = False
        self.joining = False
        self.ip = None
        self.t = None  # rejoin on the next poll, no retry delay
        self.text = "reset: " + reason

    def _log(self, msg):
        if self.log is not None:
            self.log(msg)

    def stop(self):
        try:
            self.wlan.disconnect()
        except OSError:
            pass


_video_addr = None


def open_video():
    """Non-blocking TCP socket with its connect to the ESP in progress."""
    global _video_addr
    if _video_addr is None:
        # An IP literal resolves without DNS; cached so reconnects never block here.
        _video_addr = socket.getaddrinfo(P.ESP_ADDR, P.VIDEO_PORT)[0][-1]
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.setblocking(False)
        if hasattr(socket, "TCP_NODELAY"):
            # One-byte credits must not wait for Nagle.
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.connect(_video_addr)
    except OSError as e:
        code = e.errno if getattr(e, "errno", None) is not None else (e.args[0] if e.args else -1)
        if code != errno.EINPROGRESS:
            s.close()
            raise
    return s
