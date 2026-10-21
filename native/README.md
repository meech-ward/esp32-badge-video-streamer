# Native Tufty 2350 receiver

Optional C++ firmware for the **Tufty 2350** and its 320×240 LCD. This replaces
Badgeware while installed; the [MicroPython app](../tufty/video_streamer/) keeps
stock Badgeware. Both use the same [video protocol](../docs/protocol.md).
**B** toggles the HUD. **HOME** enters USB BOOTSEL. Other buttons are unused.

## Build and install

Install CMake, Ninja, Python 3, `picotool` and an Arm Cortex-M33 GCC toolchain.
Use **Pico SDK 2.2.0 with submodules** and the pinned JPEGDEC directory listed in
[THIRD_PARTY.md](THIRD_PARTY.md). From the repository root, generate matching
private credentials with `python3 tools/make_secrets.py`; if credentials already
exist, copy `native/secrets.example.h` to ignored `native/secrets.h` and match
the camera hotspot.

Fetch the pinned sources into an ignored local directory if needed:

```sh
mkdir -p native/.deps
git clone --branch 2.2.0 --recurse-submodules \
  https://github.com/raspberrypi/pico-sdk.git native/.deps/pico-sdk
git clone https://github.com/pimoroni/picovector.git native/.deps/picovector
git -C native/.deps/picovector checkout 9fdf8e62bcca93f8d53f8ba9278d667bc6a058dd
```

```sh
cd native
cmake --preset production \
  -DPICO_SDK_PATH="$PWD/.deps/pico-sdk" \
  -DPICO_TOOLCHAIN_PATH=/path/to/arm-gnu-toolchain \
  -DJPEGDEC_PATH="$PWD/.deps/picovector/lib/jpegdec"
cmake --build --preset production
```

Put the intended badge into BOOTSEL, then select its serial explicitly:

```sh
picotool load -v -x build/production/native_video_streamer.uf2 --ser BADGE_SERIAL
```

The build checks every UF2 write. Ordinary writes stay below the last sector
of the first 2 MiB, with the RP2350-E10 compatibility block at `0x101fff00`.
This preserves ROMFS and user storage for the verified Badgeware layout
(2 MiB firmware, 1 MiB ROMFS, 13 MiB user filesystem). Other layouts require
separate verification. This does not restore Badgeware; reinstall stock
firmware to use its apps again.

## Pipeline

The production preset requests **320×240, JPEG quality 6 and one credit**.
Core 0 owns Wi-Fi, buttons and LCD DMA; core 1 decodes JPEGs. Two RGB565 images
stay in SRAM. Two guarded 64 KiB PSRAM inputs keep the decoder's input immutable
while receiving the next frame. The previous successful decode duration allows
one next-frame credit up to 12 ms before completion; the first frame of every
TCP session waits until decoding ends. There is no playback queue.

The native RGB565 M33 DSP decoder, hot-code SRAM placement, 16-bit LCD DMA,
10 ms TCP timer and 8 ms data-only CYW43 TX bound come from the optimized RC
receiver. Build-local dependency adaptations leave upstream checkouts unchanged.
The shorter TCP timer is intended for this dedicated hotspot, not general
Internet traffic. Late or obsolete-session frames are discarded; input guards,
a watchdog, TCP retry and asynchronous Wi-Fi recovery remain active.

The HUD displays presented FPS, JPEG decode time and the latest display gap.
These are not complete camera-to-screen latency measurements. Defaults emit
one compact USB health record every two seconds only while a host is connected;
USB writes never wait. Timing histograms and frame traces are compiled out.
The `diagnostic` preset enables timing records in a separate build directory.
Optional gap tracing additionally uses `NATIVE_TRACE_GAPS=ON`; TCP ingress
tracing requires that option and `NATIVE_TRACE_TCP_INGRESS=ON`.

Host checks run with `uv run pytest native/tests -q`. Set `JPEGDEC_PATH` and
install Pillow to include generated JPEG color/bounds tests. The port is
build/host tested; its video-only firmware has not been flashed or physically
qualified here. The related RC firmware's hardware measurements do not establish
this variant's frame rate or latency.

Build outputs contain Wi-Fi credentials. Keep `native/secrets.h`, build folders
and diagnostic captures private. Dependency licenses are listed in
[THIRD_PARTY.md](THIRD_PARTY.md).
