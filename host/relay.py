#!/usr/bin/env python3
"""Watch the ESP's video in a browser.

  uv run python -m host.relay                       # ESP at 192.168.4.1
  open http://127.0.0.1:8090/

Connects once to the ESP as its single video client (so the badge cannot be
connected at the same time: a new video client replaces the old one) and
re-serves the frames as MJPEG (multipart/x-mixed-replace). Endpoints:
/ (index), /stream, /frame.jpg, /status.json. No control channel is opened.
"""
import argparse
import json
import pathlib
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

_ROOT = pathlib.Path(__file__).resolve().parent.parent
for _p in (_ROOT, _ROOT / "tufty/video_streamer"):
    if str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

import stream_protocol as P  # noqa: E402
from host.video import VideoClient, parse_size  # noqa: E402

BOUNDARY = "videoframe"
INDEX = b"""<!doctype html><meta charset="utf-8"><title>ESP32 badge video streamer</title>
<style>body{background:#111;color:#ccc;font:14px system-ui;margin:16px}
img{image-rendering:pixelated;width:min(100%,640px);background:#000}</style>
<h1>ESP32 badge video streamer</h1><img src="/stream" alt="video"><pre id="s"></pre>
<script>setInterval(()=>fetch('/status.json').then(r=>r.json())
.then(j=>document.getElementById('s').textContent=JSON.stringify(j,null,1)),1000)</script>
"""


class Relay:
    def __init__(self, host, video_port=P.VIDEO_PORT, framesize=P.FS_QVGA, quality=6, credits=1,
                 verbose=True):
        self.host, self.video_port = host, video_port
        self.framesize, self.quality, self.credits = framesize, quality, credits
        self.verbose = verbose
        self.cond = threading.Condition()
        self.frame = None  # newest JPEG
        self.frame_id = 0
        self.status = {}
        self.connected = False
        self.stop_flag = False
        self.thread = threading.Thread(target=self._run, daemon=True, name="relay-video")

    def log(self, msg):
        if self.verbose:
            print(f"[relay] {msg}", file=sys.stderr, flush=True)

    def start(self):
        self.thread.start()
        return self

    def stop(self):
        self.stop_flag = True
        with self.cond:
            self.cond.notify_all()

    def _run(self):
        backoff = 0.5
        while not self.stop_flag:
            client = None
            try:
                client = VideoClient(self.host, self.video_port, timeout=3.0)
                client.sock.settimeout(3.0)
                client.hello(self.framesize, self.quality, self.credits)
                self.connected, backoff = True, 0.5
                self.log(f"connected to {self.host}:{self.video_port}")
                while not self.stop_flag:
                    msg = client.read()
                    if msg.mtype == P.TYPE_STATUS:
                        try:
                            self.status = json.loads(msg.payload)
                        except ValueError:
                            pass
                        continue
                    with self.cond:
                        self.frame = msg.payload
                        self.frame_id += 1
                        self.cond.notify_all()
                    # Storing the frame is our "draw"; browsers pick the newest themselves.
                    client.credit()
            except (OSError, ConnectionError, P.ProtocolError) as e:
                if not self.stop_flag:
                    self.log(f"video: {e}; retrying in {backoff:.1f}s")
            finally:
                self.connected = False
                if client:
                    client.close()
            if not self.stop_flag:
                time.sleep(backoff)
                backoff = min(backoff * 2, 5.0)

    def wait_frame(self, after_id, timeout):
        with self.cond:
            if self.frame_id == after_id and not self.stop_flag:
                self.cond.wait(timeout)
            return self.frame_id, self.frame


def make_server(relay, bind="127.0.0.1", port=8090):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

        def _send(self, body, ctype):
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/":
                self._send(INDEX, "text/html; charset=utf-8")
            elif self.path == "/status.json":
                st = dict(relay.status, relay_connected=relay.connected, relay_frames=relay.frame_id)
                self._send(json.dumps(st).encode(), "application/json")
            elif self.path == "/frame.jpg":
                _, jpeg = relay.wait_frame(0, 2.0)
                if jpeg is None:
                    self.send_error(503, "no frame yet")
                else:
                    self._send(jpeg, "image/jpeg")
            elif self.path == "/stream":
                self._stream()
            else:
                self.send_error(404)

        def _stream(self):
            self.send_response(200)
            self.send_header("Content-Type", f"multipart/x-mixed-replace; boundary={BOUNDARY}")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            last = 0
            try:
                while not relay.stop_flag:
                    fid, jpeg = relay.wait_frame(last, 1.0)
                    if fid == last or jpeg is None:
                        continue
                    last = fid  # skips frames this browser was too slow for
                    self.wfile.write(f"--{BOUNDARY}\r\nContent-Type: image/jpeg\r\n"
                                     f"Content-Length: {len(jpeg)}\r\n\r\n".encode() + jpeg + b"\r\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, socket.timeout):
                pass

    server = ThreadingHTTPServer((bind, port), Handler)
    server.daemon_threads = True
    return server


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=P.ESP_ADDR, help="ESP address")
    ap.add_argument("--video-port", type=int, default=P.VIDEO_PORT)
    ap.add_argument("--size", default="qvga", help="qqvga, qvga, 240, vga or a frame-size code")
    ap.add_argument("--quality", type=int, default=6, choices=range(4, 64), metavar="4..63")
    ap.add_argument("--credits", type=int, default=1, choices=range(1, 5))
    ap.add_argument("--bind", default="127.0.0.1", help="HTTP bind address")
    ap.add_argument("--port", type=int, default=8090, help="HTTP port")
    args = ap.parse_args(argv)
    relay = Relay(args.host, args.video_port, parse_size(args.size), args.quality, args.credits).start()
    server = make_server(relay, args.bind, args.port)
    print(f"serving http://{args.bind}:{server.server_address[1]}/", file=sys.stderr, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        relay.stop()
        server.server_close()


if __name__ == "__main__":
    main()
