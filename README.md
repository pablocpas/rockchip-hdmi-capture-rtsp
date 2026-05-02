# Rockchip HDMI Capture RTSP Streamer

Low-latency HDMI UVC capture streamer for Rockchip boards.

It captures MJPEG from a USB HDMI capture dongle, decodes it with Rockchip MPP,
encodes H.264 with Rockchip MPP, captures ALSA audio, and serves the result as
RTSP/TCP. It is designed for small ARM boards where using FFmpeg plus a relay
server can waste too much CPU.

Tested on a NanoPi R3S LTS with RK3568 and a MacroSilicon/MS2109-style UVC HDMI
capture dongle at 1080p30.

Keywords: Rockchip MPP, RK3568, RK3588, HDMI capture, UVC capture card, RTSP,
H.264 hardware encoding, MJPEG hardware decoding, Opus audio, WireGuard.

## Features

- 1080p30 MJPEG capture from V4L2/UVC.
- Rockchip MPP JPEG decode and H.264 encode.
- Optional V4L2 `DMABUF` capture into MPP/DRM buffers with `--v4l2-dmabuf`.
- Direct RTSP/TCP interleaved server, no MediaMTX required.
- Optional MediaMTX publisher mode for compatibility.
- ALSA audio capture with Opus or L16 RTP audio.
- Multi-client direct RTSP server, intended for a small number of viewers.
- Capture starts only when an RTSP client enters `PLAY`.

## Current Limitations

- The optimized capture path targets UVC MJPEG input. Raw YUYV/YUY2 capture is
  less useful on many cheap dongles because 1080p is often limited to low FPS.
- RTSP serving is TCP interleaved only. This is intentional for stability over
  tunnels such as WireGuard.
- The direct server is designed for a few viewers, not large fan-out streaming.
- Hardware acceleration depends on the board image exposing working Rockchip MPP
  headers, libraries, and runtime support.

## Hardware Requirements

- Rockchip board with MPP support, for example RK3568/RK3588-class boards.
- UVC HDMI capture dongle that exposes MJPEG at the target resolution/FPS.
- USB bandwidth for the selected capture mode.

## Installation

The easiest path is to install a prebuilt `linux-aarch64` release binary. This
does not require build tools, CMake, compiler packages, or development headers.

### Runtime Requirements

Install runtime tools and shared libraries:

```bash
sudo apt update
sudo apt install -y \
  v4l-utils alsa-utils \
  libv4l-0 libturbojpeg0 libasound2 libopus0
```

You also need Rockchip MPP runtime libraries from your board vendor image or
packages. Package names vary; on some images they are included by default.

### Install From Release Binary

Download the latest `linux-aarch64` tarball from GitHub Releases, then:

```bash
tar -xzf rockchip-hdmi-capture-rtsp-*-linux-aarch64.tar.gz
cd rockchip-hdmi-capture-rtsp-*-linux-aarch64
sudo install -m 755 bin/rk-hdmi-streamer /usr/local/bin/rk-hdmi-streamer
```

Install the direct RTSP service:

```bash
sudo install -m 644 systemd/rk-hdmi-streamer.env /etc/default/rk-hdmi-streamer
sudo install -m 644 systemd/rk-hdmi-streamer-direct.service /etc/systemd/system/rk-hdmi-streamer.service
sudo systemctl daemon-reload
sudo systemctl enable --now rk-hdmi-streamer.service
```

The release binary is dynamically linked and built for Linux `aarch64`. It still
requires compatible runtime libraries on the target Rockchip system.

## Build Instructions

Only follow this section if you want to compile from source or create your own
release binary.

### Build Requirements

Install build tools and development headers:

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config git \
  libv4l-dev libturbojpeg0-dev libasound2-dev libopus-dev
```

You also need Rockchip MPP development headers and pkg-config metadata, usually
provided by a vendor package such as `librockchip-mpp-dev` or the board image.
Check with:

```bash
pkg-config --modversion rockchip_mpp
```

### Build From Source

```bash
git clone https://github.com/pablocpas/rockchip-hdmi-capture-rtsp.git
cd rockchip-hdmi-capture-rtsp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Install the binary:

```bash
sudo cmake --install build
```

This installs `rk-hdmi-streamer` to `/usr/local/bin` by default.

### Build A Release Tarball

To build a release tarball from a Rockchip board:

```bash
scripts/package-release.sh v0.1.0
```

The generated files are written to `dist/`:

```text
rockchip-hdmi-capture-rtsp-v0.1.0-linux-aarch64.tar.gz
rockchip-hdmi-capture-rtsp-v0.1.0-linux-aarch64.tar.gz.sha256
```

## Find Capture Devices

List video devices and formats:

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --list-formats-ext
```

List ALSA capture devices:

```bash
arecord -l
```

Many cheap HDMI capture dongles expose 1080p30 only as MJPEG. YUYV/YUY2 may be
limited to lower frame rates because it requires much more USB bandwidth.

## Direct RTSP Server

Recommended mode:

```bash
rk-hdmi-streamer \
  --decoder mppjpeg \
  --v4l2-dmabuf \
  --device /dev/video0 \
  --audio-device plughw:CARD=MS2109,DEV=0 \
  --audio-codec opus \
  --audio-gain 3.0 \
  --width 1920 \
  --height 1080 \
  --fps 30 \
  --bitrate 12000000 \
  --gop 30 \
  --rtp-payload 1400 \
  --listen-rtsp :8554 \
  --rtsp-path /capture \
  --max-clients 3
```

Open:

```text
rtsp://BOARD_IP:8554/capture
```

For local testing:

```bash
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/capture
```

## Install As A Service

Install the direct RTSP service:

```bash
sudo install -m 644 systemd/rk-hdmi-streamer.env /etc/default/rk-hdmi-streamer
sudo install -m 644 systemd/rk-hdmi-streamer-direct.service /etc/systemd/system/rk-hdmi-streamer.service
sudo systemctl daemon-reload
sudo systemctl enable --now rk-hdmi-streamer.service
```

Edit defaults if your device names differ:

```bash
sudo nano /etc/default/rk-hdmi-streamer
sudo systemctl restart rk-hdmi-streamer.service
```

Check status:

```bash
sudo systemctl status rk-hdmi-streamer.service --no-pager -l
```

If MediaMTX is installed and uses port `8554`, stop it first:

```bash
sudo systemctl disable --now mediamtx.service
```

The provided service runs as user `pi`. If your board uses another user, edit
`User=` and `Group=` in the service file before installing it.

## MediaMTX Compatibility Mode

Direct RTSP is preferred, but publishing to MediaMTX is still supported:

```bash
rk-hdmi-streamer \
  --decoder mppjpeg \
  --v4l2-dmabuf \
  --device /dev/video0 \
  --audio-device plughw:CARD=MS2109,DEV=0 \
  --audio-codec opus \
  --width 1920 \
  --height 1080 \
  --fps 30 \
  --bitrate 12000000 \
  --gop 30 \
  --rtp-payload 1400 \
  --output rtsp://127.0.0.1:8554/capture
```

There is also a legacy systemd unit at `systemd/rk-hdmi-streamer.service` for
this mode.

## File Output

Raw Annex-B H.264 output is useful for debugging:

```bash
rk-hdmi-streamer \
  --decoder mppjpeg \
  --v4l2-dmabuf \
  --device /dev/video0 \
  --no-audio \
  --width 1920 \
  --height 1080 \
  --fps 30 \
  --bitrate 12000000 \
  --frames 300 \
  --output /tmp/capture.h264
```

## Network Notes

The direct server uses RTSP/TCP interleaved RTP. This is usually the most stable
choice over WireGuard because it avoids UDP-over-UDP loss amplification and works
well with BBR on the outer TCP flow.

For Linux TCP tuning, BBR plus `fq` is a reasonable starting point:

```bash
sudo tee /etc/sysctl.d/99-rk-streaming.conf >/dev/null <<'EOF'
net.ipv4.tcp_congestion_control=bbr
net.core.default_qdisc=fq
net.ipv4.tcp_slow_start_after_idle=0
EOF
sudo sysctl --system
```

## Measured Efficiency

Measured on a NanoPi R3S LTS with RK3568, 1080p30 MJPEG input, Rockchip MPP JPEG
decode, Rockchip MPP H.264 encode, Opus audio, `--bitrate 12000000`, and one
RTSP/TCP client.

The best tested direct RTSP configuration was:

```bash
rk-hdmi-streamer \
  --decoder mppjpeg \
  --v4l2-dmabuf \
  --device /dev/video0 \
  --audio-device plughw:CARD=MS2109,DEV=0 \
  --audio-codec opus \
  --audio-gain 3.0 \
  --width 1920 \
  --height 1080 \
  --fps 30 \
  --bitrate 12000000 \
  --gop 30 \
  --rtp-payload 1400 \
  --listen-rtsp :8554 \
  --rtsp-path /capture \
  --max-clients 3
```

Observed results:

| Mode | CPU, one core basis | CPU, four-core basis | Notes |
| --- | ---: | ---: | --- |
| MediaMTX relay mode | about 76-89% | about 19-22% | streamer plus MediaMTX process |
| Direct RTSP server | about 51-53% | about 13% | streamer only, one client |

The direct RTSP server removed roughly 25-35 percentage points of one CPU core
versus the MediaMTX relay path in this setup. MediaMTX was not doing heavy
transcoding, but it still added process, socket, RTP forwarding, and buffering
overhead that is avoidable for this small-client use case.

During the direct RTSP test, network output was around 12 Mbps with Opus audio
enabled and no queue drops. With `--rtp-payload 1400`, packetization overhead and
`sendmsg` calls are reduced compared with the earlier 1200-byte RTP payload.

These numbers are workload-specific. CPU usage changes with bitrate, client
count, kernel, MPP version, capture dongle, memory clocks, and whether the stream
is viewed through a tunnel.

To measure your own board, compare process CPU over a short interval while a
client is actively playing:

```bash
pid="$(pgrep -n rk-hdmi-streamer)"
ps -p "$pid" -o pid,comm,%cpu,%mem,args
```

For more detail, use `top`, `htop`, or `perf top` while streaming.

## Troubleshooting

If video is black or has artifacts:

- Confirm the dongle supports the requested mode with `v4l2-ctl`.
- Try without `--v4l2-dmabuf`; some UVC drivers expose incompatible buffers.
- Lower bitrate temporarily and check system logs.

If audio is missing:

- Check `arecord -l` and adjust `AUDIO_DEVICE`.
- Try `--audio-codec l16` to isolate Opus-related issues.
- Increase `--audio-gain` if the source is quiet.

If the service does not start:

```bash
journalctl -u rk-hdmi-streamer.service -n 100 --no-pager
```

## License

MIT.
