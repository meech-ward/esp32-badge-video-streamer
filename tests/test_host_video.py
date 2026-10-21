"""Actual desktop parser and HTTP relay exercised without a camera."""
import json
import socket
import threading
import urllib.request

import pytest

import stream_protocol as P
from host.relay import Relay, make_server
from host.video import VideoClient


def test_desktop_parser_resumes_after_partial_header_and_payload_timeout():
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    client = VideoClient("127.0.0.1", listener.getsockname()[1], timeout=.05)
    peer, _ = listener.accept()
    try:
        client.hello(P.FS_QVGA, 6, 1)
        assert peer.recv(P.HELLO_SIZE) == P.pack_hello(P.FS_QVGA, 6, 1)
        packet = P.pack_header(P.TYPE_FRAME, 17, 320, 4, 1234) + b"jpeg"
        peer.sendall(packet[:5])
        with pytest.raises(socket.timeout):
            client.read()
        peer.sendall(packet[5:-2])
        with pytest.raises(socket.timeout):
            client.read()
        peer.sendall(packet[-2:])
        message = client.read()
        assert tuple(message) == (P.TYPE_FRAME, 17, 320, b"jpeg", 1234)
        client.credit()
        assert peer.recv(1) == P.CREDIT
        peer.sendall(b"XC" + packet[2:P.HEADER_SIZE])
        with pytest.raises(P.ProtocolError):
            client.read()
    finally:
        client.close()
        peer.close()
        listener.close()


def test_http_viewers_share_latest_frame_without_extra_camera_connections():
    relay = Relay("unused", verbose=False)
    relay.frame, relay.frame_id, relay.connected = b"jpeg", 1, True
    relay.status = {"fps": 34}
    server = make_server(relay, port=0)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    url = "http://127.0.0.1:%d" % server.server_address[1]
    try:
        for _ in range(2):
            with urllib.request.urlopen(url + "/frame.jpg", timeout=2) as response:
                assert response.headers["Content-Type"] == "image/jpeg"
                assert response.read() == b"jpeg"
        with urllib.request.urlopen(url + "/status.json", timeout=2) as response:
            status = json.load(response)
            assert status["relay_connected"] is True and status["fps"] == 34
        with urllib.request.urlopen(url + "/stream", timeout=2) as response:
            assert response.readline().strip() == b"--videoframe"
            assert response.readline().strip() == b"Content-Type: image/jpeg"
            assert response.readline().strip() == b"Content-Length: 4"
            assert response.readline().strip() == b""
            assert response.read(4) == b"jpeg"
        assert not relay.thread.is_alive()  # HTTP never starts another camera client.
    finally:
        relay.stop()
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
