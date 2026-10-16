# Development

## Configuration

The badge's frame settings are constants in
[`__init__.py`](../tufty/video_streamer/__init__.py). Its local configuration is
`wifi_config.py`, generated alongside the camera settings by
[`make_secrets.py`](../tools/make_secrets.py). Keep generated credentials private.

With ESP-IDF activated, `idf.py -C esp menuconfig` exposes the Wi-Fi channel,
image flip/mirror, boot size and quality. The viewer's HELLO selects the
stream size/quality. `sdkconfig.defaults` and `dependencies.lock` pin build
settings and camera dependencies; generated SDK files are ignored.

## Checks

Run `uv run pytest -q` and `make -C esp/host_test`. The C++ checks use a C++20
compiler, Python 3, and sanitizers; `CXX=clang++` selects a compiler and
`SANITIZERS=` disables sanitizer flags when unavailable.

Protocol tests optionally use `micropython` on PATH or an executable set in
`TUFTY_VIDEO_MICROPYTHON`. Graphics checks require a separately built
Badgeware v3.0.2/PicoVector playground:

```sh
BADGE_SOURCE=/path/to/badge node tools/check_emulator.mjs
```

Only networking is replaced with a test fixture. The actual viewer, stream
parser and JPEG renderer run unchanged. Generated screenshots and reports stay
under ignored `receipts/emulator/`; fixtures are never installed on the badge.

## Source and verification

Derived from the local `xiao-cam-rc` experiment on 2026-10-16, whose source had
no Git revision. The camera capture/copy, white balance, clock tuning, credit
pacing and receiver timing were retained. ESP-IDF v5.5.1 resolves esp32-camera
2.1.8 and esp_jpeg 1.3.1; stock Badgeware v3.0.2 supplies the viewer runtime.
Downloaded dependencies retain their upstream licenses and are not vendored.

Host tests, C++ sanitizer checks, the graphics emulator and ESP32-S3 compilation
pass. This standalone version still needs physical streaming validation: check
moving video, B HUD toggling, HOME exit, camera-restart recovery and device fps.
Emulator timing does not establish radio performance or camera-to-screen latency.
