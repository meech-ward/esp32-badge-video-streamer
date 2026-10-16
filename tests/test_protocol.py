"""Wire-format fixtures and malformed-input boundaries."""
import struct

import pytest

import stream_protocol as P


def test_wire_fixtures():
    assert P.pack_hello() == bytes.fromhex("5648010006010000")
    assert P.pack_header(P.TYPE_FRAME, 0x1234, 160, 999, 5) == bytes.fromhex(
        "5653010134121400e703000005000000")
    assert P.unpack_header(P.pack_header(P.TYPE_STATUS, 4, 0, 20)) == (2, 4, 0, 20, 0)


@pytest.mark.parametrize("fs", P.FRAME_SIZES)
@pytest.mark.parametrize("q", [4, 6, 63])
@pytest.mark.parametrize("credits", [1, 4])
def test_hello_supported_boundaries(fs, q, credits):
    assert P.unpack_hello(P.pack_hello(fs, q, credits)) == (fs, q, credits)


@pytest.mark.parametrize("fields", [(4, 6, 1), (0, 3, 1), (0, 64, 1), (0, 6, 0), (0, 6, 5)])
def test_hello_rejects_invalid_configuration(fields):
    with pytest.raises(P.ProtocolError):
        P.pack_hello(*fields)
    with pytest.raises(P.ProtocolError):
        P.unpack_hello(struct.pack(P.HELLO_FMT, b"VH", 1, *fields, 0))


@pytest.mark.parametrize("wire", [b"", b"VH", b"XH\1\0\6\1\0\0",
                                 b"VH\2\0\6\1\0\0", b"VH\1\0\6\1\1\0"])
def test_hello_rejects_invalid_packet(wire):
    with pytest.raises(P.ProtocolError):
        P.unpack_hello(wire)


@pytest.mark.parametrize("kind,length", [(3, 10), (P.TYPE_FRAME, P.MAX_PAYLOAD + 1),
                                        (P.TYPE_STATUS, P.STATUS_MAX + 1)])
def test_header_rejects_unknown_types_and_oversized_payloads(kind, length):
    with pytest.raises(P.ProtocolError):
        P.unpack_header(P.pack_header(kind, 1, 160, length))


@pytest.mark.parametrize("kind,length", [(P.TYPE_FRAME, P.MAX_PAYLOAD),
                                        (P.TYPE_STATUS, P.STATUS_MAX)])
def test_payload_limits_are_inclusive(kind, length):
    assert P.unpack_header(P.pack_header(kind, 1, 160, length))[3] == length
