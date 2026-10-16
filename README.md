# ESP32 badge video streamer

Live video from a **Seeed XIAO ESP32-S3 Sense** to a **Pimoroni Tufty 2350**
running stock Badgeware. The ESP32 creates a Wi-Fi hotspot; the badge joins it
and displays the camera feed. No router or Internet connection is needed.

<a href="https://x.com/wesbos/status/2111109101174624669"><img src="docs/assets/wes-bos-tweet.jpg" alt="Wes Bos (@wesbos) posting about live camera video on a conference badge." width="480"></a>

**B** toggles the FPS/statistics overlay. **HOME** exits. There are no remote
controls, photo storage, or additional badge apps.

## Requirements

- XIAO ESP32-S3 Sense with 8 MB flash, 8 MB octal PSRAM, and OV2640 or OV5640.
- Tufty 2350 with stock [Badgeware v3.0.2](https://github.com/pimoroni/tufty2350/releases/tag/v3.0.2),
  the runtime checked by this project. The viewer uses its in-memory JPEG decoder.
- Power for both devices, Python 3.10+, and
  [ESP-IDF v5.5.1](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/get-started/index.html).

## Setup

From the repository root, generate matching private Wi-Fi settings:

```sh
python3 tools/make_secrets.py
```

The hotspot is `tufty-video`. Its random password is written to ignored
`esp/main/wifi_secrets.h` and `tufty/video_streamer/wifi_config.py`.
To change it later, use `--force`, then rebuild and update both devices.

Activate your ESP-IDF environment, then build and flash **the camera** using
its serial port:

```sh
idf.py -C esp build
idf.py -C esp -p <camera-port> flash
```

Install **the badge app** while keeping stock firmware:

1. Enter Disk Mode by double-tapping RESET, then back up the mounted USB volume.
2. Copy `tufty/video_streamer/` into the volume's `apps/` directory.
   `wifi_config.example.py` is an example and can be omitted.
3. To keep only the viewer, remove other app folders while preserving
   **`apps/menu` and `apps/video_streamer`**. The stock menu is needed for startup.
4. Eject the volume and open **video_streamer** from the launcher.

On **macOS**, the installer automates copying, verification, backup of other
apps, and ejecting. It needs `mpremote` on PATH (`uv tool install mpremote`):

```sh
python3 tools/install_tufty.py --only-app \
  --backup-dir "$HOME/tufty-backups/before-video-streamer"
```

Use a new backup directory outside the badge volume. Add `--no-reset` if Disk
Mode is already mounted, `--port <badge-port>` to select a device, or
`--dry-run` to preview changes on a mounted volume. For app-only updates, omit
`--only-app` and `--backup-dir`.

## Streaming and statistics

Defaults are **160×120, JPEG quality 6, one frame of credit**. The ESP keeps
capturing into PSRAM and sends the latest JPEG over TCP. The badge decodes it
directly into screen RAM and immediately returns a credit, allowing transfer
to overlap display work. Old captures are dropped instead of accumulating.
Preallocated buffers, bounded reads, scheduled garbage collection, and stale
link detection keep memory use predictable and allow automatic reconnection.
Software white balance and the OV5640 low-resolution clock tuning are retained.

The HUD starts visible: `fps` is displayed frames/second; `n` receive time,
`d` decode time, `s` display/input time, `g` garbage collection time, and `a`
capture-to-send age are in milliseconds; `k` is JPEG size in KiB. Frame age is
only part of camera-to-screen latency. Connection messages remain visible with
the HUD hidden. See the [wire protocol](docs/protocol.md) for transport details.

## Development

```sh
uv sync
uv run pytest -q
make -C esp/host_test
```

`esp/` contains camera firmware, `tufty/video_streamer/` the installable app,
and `tools/` setup helpers. Tests and their fixtures stay on the host.
See [development notes](docs/development.md) for configuration, optional graphics
checks, and verification limits.

[MIT licensed](LICENSE).
