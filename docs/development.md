# Development

## Configuration

Run `python3 tools/make_secrets.py` from the repository root. It creates matching
ignored camera, Badgeware and native Wi-Fi settings without printing the password.
Existing files require `--force` to regenerate the complete set. Firmware binaries
embed credentials; keep generated configuration and build artifacts private.

The stock app's frame settings and `SERIAL_DIAGNOSTICS` switch are in
[`__init__.py`](../tufty/video_streamer/__init__.py); USB logging defaults off.
ESP-IDF `menuconfig` exposes channel, flip/mirror, boot size/quality, capture
buffer count and optional anti-flicker profiles. The viewer's HELLO selects its
stream size/quality. Default camera logs are WARN/ERROR through a bounded queue;
there is no USB command parser or snapshot buffer.

`esp/sdkconfig.defaults` and `esp/dependencies.lock` specify ESP-IDF 5.5.1,
esp32-camera 2.1.8 and esp_jpeg 1.3.1. Defaults do not overwrite an existing
`esp/sdkconfig`; regenerate it after changing defaults. The default anti-flicker
profile retains the driver timing; corrected 50/60 Hz modes are optional, since
they can change dim-scene exposure. Choose based on the light source.

The camera build stages checked dependency copies: a one-entry latest-frame
queue, first structural JPEG endpoint and timed getter prevent stale tails or
malformed frames from extending the video task's wait budget. Exact upstream
hash checks reject unreviewed revisions. Managed dependency sources stay unchanged.

## Badgeware installation

Stock Badgeware v3.0.2 supplies the in-memory JPEG decoder. Copy only
`tufty/video_streamer/` into its mounted `apps/` folder; host tools/tests/native
sources do not belong on the badge. Keep `apps/menu` for startup.

On macOS, install `mpremote` (`uv tool install mpremote`), then use:

```sh
python3 tools/install_tufty.py
```

To remove other apps while preserving the menu, explicitly use `--only-app`
with a new backup directory outside the badge volume:

```sh
python3 tools/install_tufty.py --only-app \
  --backup-dir "$HOME/tufty-backups/before-video-streamer"
```

`--port <badge-port>` selects a badge; `--no-reset` uses an already mounted Disk
Mode volume; `--dry-run` previews changes on a mounted volume. The native receiver
has a separate [build/install guide](../native/README.md).

## Checks

```sh
uv sync
uv run pytest -q
make -C esp/host_test
```

C++ host checks use a C++20 compiler and sanitizers. `CXX=clang++` selects a
compiler; `SANITIZERS=` disables sanitizer flags where unavailable. Native
portable tests use C++17 and ASAN/UBSAN. Set `JPEGDEC_PATH` to the pinned source
directory in [THIRD_PARTY.md](../native/THIRD_PARTY.md) for real JPEG color/bounds
checks. Camera driver tests use the managed dependency installed by an ESP build;
otherwise those checks are explicitly skipped.

Protocol tests optionally use `micropython` or `TUFTY_VIDEO_MICROPYTHON`.
Graphics checks need a separately built Badgeware v3.0.2/PicoVector playground:

```sh
BADGE_SOURCE=/path/to/badge node tools/check_emulator.mjs
```

Only networking is replaced with a fixture. The real app, parser and JPEG
renderer run unchanged. Generated screenshots remain in ignored
`receipts/emulator/`; fixtures are never installed on the badge.

## Provenance and limits

Originally extracted from the RC experiment on 2026-10-16; the 2026-10-21 port
adds its bounded capture/JPEG fixes, production logging, optimized native video
pipeline and Mac relay without motor/control code. The related source now lives
in [ESP32 badge RC](https://github.com/meech-ward/esp32-badge-rc).
The protocols deliberately have different magic/header formats; do not mix
firmware between the two projects.

Build/host checks establish parsing, memory ownership, dependency staging and
configuration consistency. The video-only port has not been flashed in this
publication task. Physical checks still need moving video, B/HOME, restart/link
recovery, image quality and sustained frame intervals. Emulator or RC measurements
do not establish this variant's RF behavior, FPS or glass-to-glass latency.
Fine low-light vertical columns seen in RC source-camera JPEGs have no general
software cure established here.
