# Rockchip HDMI Capture RTSP Streamer

Low-latency HDMI UVC capture streamer for Rockchip boards.

It captures video from MacroSilicon-style HDMI USB capture dongles, converts it
with Rockchip MPP/RGA when available, encodes H.264 with Rockchip MPP, captures
ALSA audio, and serves a direct RTSP/TCP stream. It is intended for small ARM
boards where a full FFmpeg plus relay-server pipeline wastes too much CPU.

## What It Supports

- Direct RTSP/TCP interleaved server, no relay required.
- H.264 video from Rockchip MPP encoder.
- MJPEG capture path: UVC MJPEG -> MPP JPEG -> MPP H.264.
- RGA capture path: UVC YUYV -> RGA NV12 -> MPP H.264.
- CPU fallback path: UVC YUYV -> libyuv NV12 -> MPP H.264.
- ALSA audio with Opus or L16 RTP payloads.
- On-demand capture: the device starts when an RTSP client enters `PLAY`.
- Small multi-client use, typically a few viewers.
- Optional legacy MediaMTX publisher mode.

## Recommended Profiles

Use the interactive configurator unless you already know the exact mode your
dongle exposes:

```bash
sudo scripts/configure.sh
scripts/doctor.sh
```

The profiles are:

| Profile | Pipeline | Use when |
| --- | --- | --- |
| `rga` | YUYV DMA-BUF -> RGA NV12 -> MPP H.264 | Best-performance path when YUYV mode and `librga.so` are available |
| `mjpeg` | MJPEG DMA-BUF -> MPP JPEG -> MPP H.264 | Safest fallback and default for most dongles |
| `yuyv-libyuv` | YUYV -> CPU libyuv NV12 -> MPP H.264 | Debug/fallback when RGA is unavailable |

`scripts/configure.sh` offers goal-oriented choices:

| Choice | Behavior |
| --- | --- |
| Auto | Keeps the current resolution/FPS when valid; prefers RGA if possible, otherwise MJPEG |
| Best performance | Chooses the best common YUYV mode for RGA |
| Most compatible | Chooses the best common MJPEG mode |
| TV cadence | Prefers 1080p50, then 720p50, then 1080p30 |
| Custom | Manual resolution, FPS, bitrate, profile, and paths |

## Tested Hardware

Tested most heavily on:

- NanoPi R3S LTS / RK3568.
- MacroSilicon MS2131-style USB3 HDMI capture dongle.
- MacroSilicon MS2109-style HDMI capture dongle.

Observed behavior:

| Device class | Typical modes | Recommendation |
| --- | --- | --- |
| MS2131 / USB3 | 1080p50/60 in `MJPG` and often `YUYV` | Prefer `rga` when RGA is installed; use `mjpeg` as safe fallback |
| MS2109 / USB2 | often no `YUYV 1920x1080@50`; 1080p may be lower FPS | Use `mjpeg` at the best advertised mode |

For European TV/set-top-box use, 50 fps usually preserves motion cadence better
than 30 fps. For cameras, consoles, desktop capture, or other HDMI sources,
choose an FPS that matches the source and appears in `v4l2-ctl --list-formats-ext`.

## Quick Start From Release

Install runtime tools and libraries:

```bash
sudo apt update
sudo apt install -y v4l-utils alsa-utils libv4l-0 libturbojpeg0 libasound2 libopus0
```

You also need Rockchip MPP runtime libraries from the board image or vendor
packages. Many Rockchip images include them already.

Install the release tarball:

```bash
tar -xzf rockchip-hdmi-capture-rtsp-*-linux-aarch64.tar.gz
cd rockchip-hdmi-capture-rtsp-*-linux-aarch64
sudo install -m 755 bin/rk-hdmi-streamer /usr/local/bin/rk-hdmi-streamer
sudo install -m 644 systemd/rk-hdmi-streamer.env /etc/default/rk-hdmi-streamer
sudo install -m 644 systemd/rk-hdmi-streamer-direct.service /etc/systemd/system/rk-hdmi-streamer.service
```

Configure and start:

```bash
sudo scripts/configure.sh
sudo systemctl daemon-reload
sudo systemctl enable --now rk-hdmi-streamer.service
scripts/doctor.sh
```

Open:

```text
rtsp://BOARD_IP:8554/capture
```

Run `sudo scripts/configure.sh` again whenever you change dongle, resolution,
FPS, bitrate, audio device, RTSP path, or profile. If the service is installed,
the configurator offers to restart it after writing the config.

## RGA Runtime

RGA is optional but recommended for the lowest CPU path when the capture dongle
advertises a matching YUYV mode.

If the release tarball includes `third_party/librga/aarch64/librga.so`, either
install it system-wide:

```bash
sudo install -m 755 third_party/librga/aarch64/librga.so /usr/local/lib/librga.so
sudo ldconfig
```

or point the config at it:

```bash
RGA_LIBRARY=/path/to/librga.so
```

The main binary loads `librga.so` only when `STREAM_PROFILE=rga` is selected, so
systems without RGA can still use `mjpeg` and `yuyv-libyuv`.

## Configuration Files

The systemd service reads:

```text
/etc/default/rk-hdmi-streamer
```

Useful keys:

```bash
DEVICE=/dev/v4l/by-id/usb-MACROSILICON_USB3_Video_20210623-video-index0
AUDIO_DEVICE=hw:CARD=Video,DEV=0
STREAM_PROFILE=mjpeg
WIDTH=1920
HEIGHT=1080
FPS=50
BITRATE=18000000
GOP=50
LISTEN_RTSP=:8554
RTSP_PATH=/capture
RTSP_DEBUG=0
```

Prefer `/dev/v4l/by-id/...video-index0` over `/dev/videoN`; the numeric node can
change after reboot or replug.

## Diagnostics

Run:

```bash
scripts/doctor.sh
```

It checks:

- installed config
- V4L2 capture nodes, excluding metadata nodes
- whether the requested format/resolution/FPS exists
- ALSA capture device
- RGA runtime availability
- RTSP port state
- `rk-hdmi-streamer.service`
- `mediamtx.service` conflicts

Manual checks:

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/v4l/by-id/usb-MACROSILICON_USB3_Video_20210623-video-index0 --list-formats-ext
arecord -l
journalctl -u rk-hdmi-streamer.service -n 100 --no-pager
```

## Build From Source

Install build dependencies:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config git \
  libv4l-dev libturbojpeg0-dev libasound2-dev libopus-dev
```

You also need Rockchip MPP development headers and pkg-config metadata, usually
from your board image or a package such as `librockchip-mpp-dev`.

Build:

```bash
git clone https://github.com/pablocpas/rockchip-hdmi-capture-rtsp.git
cd rockchip-hdmi-capture-rtsp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo cmake --install build
```

Build with RGA headers from Rockchip's `librga` checkout:

```bash
git clone --depth=1 https://github.com/airockchip/librga.git ~/librga
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRGA_ROOT="$HOME/librga"
cmake --build build -j"$(nproc)"
```

## Build A Release Tarball

Without bundled RGA:

```bash
scripts/package-release.sh v0.2.0
```

With bundled RGA runtime:

```bash
RGA_ROOT="$HOME/librga" scripts/package-release.sh v0.2.0
```

The generated files are written to `dist/`:

```text
rockchip-hdmi-capture-rtsp-v0.2.0-linux-aarch64.tar.gz
rockchip-hdmi-capture-rtsp-v0.2.0-linux-aarch64.tar.gz.sha256
```

## Manual Run Examples

Direct RTSP, compatible MJPEG path:

```bash
rk-hdmi-streamer \
  --stream-profile mjpeg \
  --device /dev/v4l/by-id/usb-MACROSILICON_USB3_Video_20210623-video-index0 \
  --audio-device hw:CARD=Video,DEV=0 \
  --audio-codec opus \
  --audio-gain 3.0 \
  --audio-frame-ms 20 \
  --width 1920 \
  --height 1080 \
  --fps 50 \
  --bitrate 18000000 \
  --gop 50 \
  --rtp-payload 1400 \
  --listen-rtsp :8554 \
  --rtsp-path /capture \
  --max-clients 3
```

Direct RTSP, best-performance RGA path:

```bash
RGA_LIBRARY="$HOME/librga/libs/Linux/gcc-aarch64/librga.so" \
rk-hdmi-streamer \
  --stream-profile rga \
  --device /dev/v4l/by-id/usb-MACROSILICON_USB3_Video_20210623-video-index0 \
  --audio-device hw:CARD=Video,DEV=0 \
  --audio-codec opus \
  --audio-gain 3.0 \
  --audio-frame-ms 20 \
  --width 1920 \
  --height 1080 \
  --fps 50 \
  --bitrate 18000000 \
  --gop 50 \
  --listen-rtsp :8554 \
  --rtsp-path /capture
```

Raw H.264 file for debugging:

```bash
rk-hdmi-streamer \
  --stream-profile mjpeg \
  --device /dev/v4l/by-id/usb-MACROSILICON_USB3_Video_20210623-video-index0 \
  --no-audio \
  --width 1920 \
  --height 1080 \
  --fps 50 \
  --bitrate 18000000 \
  --frames 300 \
  --output /tmp/capture.h264
```

## Network Notes

The direct server uses RTSP/TCP interleaved RTP. This is usually stable over
tunnels such as WireGuard because it avoids UDP-over-UDP loss amplification.

For Linux TCP tuning, BBR plus `fq` is a reasonable starting point:

```bash
sudo tee /etc/sysctl.d/99-rk-streaming.conf >/dev/null <<'EOF'
net.ipv4.tcp_congestion_control=bbr
net.core.default_qdisc=fq
net.ipv4.tcp_slow_start_after_idle=0
EOF
sudo sysctl --system
```

## Troubleshooting

If video is black or has artifacts:

- Run `scripts/doctor.sh`.
- Confirm the dongle supports the requested mode with `v4l2-ctl`.
- Try `STREAM_PROFILE=mjpeg` first.
- If using RGA, confirm `RGA_LIBRARY` points to a valid `librga.so`.
- Lower bitrate temporarily and check system logs.

If audio is missing:

- Check `arecord -l` and adjust `AUDIO_DEVICE`.
- Try `AUDIO_CODEC=l16` to isolate Opus/client issues.
- Increase `AUDIO_GAIN` if the source is quiet.
- Use `AUDIO_FRAME_MS=20` for normal Opus RTP frame size.
- On VLC for Android, if audio works the first time but not after reopening the
  same RTSP stream, change VLC's advanced audio output from `AudioTrack` to
  `OpenSL ES`.

If the service does not start:

```bash
journalctl -u rk-hdmi-streamer.service -n 100 --no-pager
```

## Notes

- Direct RTSP is preferred for this small-client use case.
- MediaMTX publisher mode is still available through `--output rtsp://...` and
  the legacy `systemd/rk-hdmi-streamer.service` unit.
- CPU usage depends on bitrate, client count, kernel, MPP version, capture
  dongle, memory clocks, and whether the stream goes through a tunnel.

## License

MIT.
