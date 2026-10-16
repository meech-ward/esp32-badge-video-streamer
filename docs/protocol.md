# Video protocol v1

One TCP connection to **192.168.4.1:8081** carries JPEG frames and periodic
status messages. A new viewer replaces the previous connection. All integers
are little-endian. Python's canonical definition is
[`stream_protocol.py`](../tufty/video_streamer/stream_protocol.py); the
[C++ mirror](../esp/components/protocol/include/streamer/protocol.hpp) is
checked against Python-generated test vectors.

## Viewer → camera

Send this eight-byte **HELLO** once after connecting:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | 2 bytes | `VH` |
| 2 | u8 | Version, 1 |
| 3 | u8 | Size: 0 = 160×120, 1 = 320×240, 2 = 240×240, 3 = 640×480 |
| 4 | u8 | JPEG quality, 4–63; lower means better quality and larger frames |
| 5 | u8 | Initial credits, 1–4 |
| 6 | u16 | Reserved; zero |

The badge defaults to size 0, quality 6, and one credit. Other sizes are
available in the camera protocol; the viewer uses 160×120 LORES.

Each subsequent byte must be ASCII **`C`**, granting one additional frame.
The badge returns credit immediately after decoding, even after a decode error.
Outstanding credits are capped at four. Invalid HELLO or credit bytes close
the connection. No button events are transmitted.

## Camera → viewer

Each message has a sixteen-byte header followed by its payload:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | 2 bytes | `VS` |
| 2 | u8 | Version, 1 |
| 3 | u8 | Type: 1 = FRAME, 2 = STATUS |
| 4 | u16 | Wrapping message sequence |
| 6 | u16 | Image width divided by 8; zero for STATUS |
| 8 | u32 | Payload byte length |
| 12 | u32 | Capture-to-send age, microseconds; zero for STATUS |

**FRAME** contains a baseline JPEG and spends one credit. The sender selects
the newest capture rather than queuing older frames. Payloads are limited to
128 KiB. The badge preallocates 32 KiB and drains/drops larger frames before
returning credit.

**STATUS** is up to 512 bytes of UTF-8 JSON, sent about once a second even
without frame credits. Optional diagnostics are `fps`, `cam_fps`, `kb`, `heap`,
`psram`, `uptime_s`, `sensor`, `fs`, `q`, and `wb`.

## Recovery

The badge retries a pending TCP connect after one second. It resets stale
Wi-Fi after 2.5 seconds without received bytes, or eight seconds if nothing
has arrived since joining. One receive poll is allowed after a long app stall
before declaring the link dead. The ESP bounds camera waits and send stalls
so a stalled sensor or disconnected viewer does not block recovery indefinitely.
