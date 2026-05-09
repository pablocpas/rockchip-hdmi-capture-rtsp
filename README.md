# Rockchip HDMI Capture RTSP Streamer

Low-latency HDMI UVC capture streamer for Rockchip boards.

It captures MJPEG from a USB HDMI capture dongle, decodes it with Rockchip MPP,
encodes H.264 with Rockchip MPP, captures ALSA audio, and serves the result as
RTSP/TCP. It is designed for small ARM boards where using FFmpeg plus a relay
server can waste too much CPU.

Tested on a NanoPi R3S LTS with RK3568 and a MacroSilicon MS2131-style USB3 UVC
HDMI capture dongle. The recommended TV/European cadence configuration is
1080p50.

## Features

- 1080p50 MJPEG capture from V4L2/UVC.
- Optional YUYV/YUY2 capture path with `libyuv` or Rockchip RGA conversion.
- Rockchip MPP JPEG decode and H.264 encode.
- Optional V4L2 `DMABUF` capture into MPP/DRM buffers with `--v4l2-dmabuf`.
- Direct RTSP/TCP interleaved server, no MediaMTX required.
- Optional MediaMTX publisher mode for compatibility.
- ALSA audio capture with Opus or L16 RTP audio.
- Multi-client direct RTSP server, intended for a small number of viewers.
- Capture starts only when an RTSP client enters `PLAY`.

## Current Limitations

- The default capture path targets UVC MJPEG input because it works on the
  widest range of dongles. On Rockchip systems with working RGA, YUYV/YUY2 can
  use a lower-CPU DMA-BUF path.
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

Rockchip RGA is optional. The binary can run without `librga.so` unless you set
`STREAM_PROFILE=rga` or pass `--stream-profile rga`.

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

### Optional RGA Runtime

The streamer does not require `librga.so` for the default MJPEG path. RGA is only
used when selecting:

```bash
STREAM_PROFILE=rga
```

If the release tarball includes `third_party/librga/aarch64/librga.so`, install
it with:

```bash
sudo install -m 755 third_party/librga/aarch64/librga.so /usr/local/lib/librga.so
sudo ldconfig
```

Or keep it outside the system library path and point the service at it:

```bash
RGA_LIBRARY=/path/to/librga.so
```

Bundling `librga.so` as an optional runtime file is intentional: the main binary
loads it only when RGA is requested, so systems without RGA can still use the
normal MJPEG and `libyuv` paths. The bundled RGA library is from Rockchip's
`airockchip/librga` project and is distributed under Apache-2.0; its license is
included beside the library.

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

### Optional RGA Support

RGA support is useful for raw YUYV/YUY2 capture because it can convert
`YUYV 4:2:2` to `NV12 4:2:0` through Rockchip RGA instead of doing the
conversion on the CPU. This is optional and only needed for:

```bash
--stream-profile rga
```

If your distro has `librga` packages, install those. If not, use Rockchip's
`librga` repository:

```bash
cd ~
git clone --depth=1 https://github.com/airockchip/librga.git
```

Build this project with RGA headers:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRGA_ROOT="$HOME/librga"
cmake --build build -j"$(nproc)"
```

At runtime, either install `librga.so` into the system linker path, or point the
streamer at the library:

```bash
export RGA_LIBRARY="$HOME/librga/libs/Linux/gcc-aarch64/librga.so"
```

The binary loads `librga.so` at runtime only when the `rga` profile is
selected. This keeps MJPEG and `libyuv` modes usable on systems without RGA.

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

To include RGA support in the release binary when `librga` is checked out
locally:

```bash
RGA_ROOT="$HOME/librga" scripts/package-release.sh v0.1.0
```

When `RGA_ROOT` is set, the release package also includes:

```text
third_party/librga/aarch64/librga.so
third_party/librga/COPYING
```

The executable still loads `librga.so` dynamically at runtime only when the
`rga` profile is selected.

The generated files are written to `dist/`:

```text
rockchip-hdmi-capture-rtsp-v0.1.0-linux-aarch64.tar.gz
rockchip-hdmi-capture-rtsp-v0.1.0-linux-aarch64.tar.gz.sha256
```

## Find Capture Devices

List video devices and formats:

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/v4l/by-id/usb-MACROSILICON_USB3_Video_20210623-video-index0 --list-formats-ext
```

List ALSA capture devices:

```bash
arecord -l
```

Many cheap HDMI capture dongles expose high-FPS modes only as MJPEG. Older
MS2109-style devices often cannot expose 1080p50 over UVC, while MS2131 USB3
cards can expose 1080p50 or higher MJPEG modes. YUYV/YUY2 may still be less
useful because it requires much more USB bandwidth and needs an extra format
conversion before hardware H.264 encoding. On Rockchip, that conversion can be
done with `libyuv` on CPU or with RGA.

## Direct RTSP Server

Recommended mode:

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

The supported profiles are:

| Profile | Pipeline | Use when |
| --- | --- | --- |
| `mjpeg` | UVC MJPEG -> MPP JPEG -> MPP H.264 | Default and most compatible |
| `rga` | UVC YUYV DMA-BUF -> RGA NV12 -> MPP H.264 | Lowest CPU on Rockchip systems with working RGA |
| `yuyv-libyuv` | UVC YUYV -> libyuv NV12 -> MPP H.264 | Debug/fallback when RGA is unavailable |

To use the Rockchip RGA path:

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
  --rtp-payload 1400 \
  --listen-rtsp :8554 \
  --rtsp-path /capture \
  --max-clients 3
```

`RGA_LIBRARY` is only needed when `librga.so` is not installed in the system
linker path. This profile captures YUYV into DMA-BUF buffers, converts YUYV to
NV12 with RGA, and feeds the Rockchip MPP H.264 encoder. On the RK3568/MS2131
test system it reduced streamer process CPU versus the `yuyv-libyuv` profile
while keeping 1080p50.

To compare the raw YUYV/YUY2 CPU fallback path:

```bash
rk-hdmi-streamer \
  --stream-profile yuyv-libyuv \
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

YUYV avoids MJPEG decode, but it uses much more USB bandwidth and still needs a
YUYV 4:2:2 to NV12 4:2:0 conversion before the Rockchip H.264 encoder. The
`yuyv-libyuv` profile does that conversion on the CPU.

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

For RGA-accelerated YUYV capture, set:

```bash
STREAM_PROFILE=rga
RGA_LIBRARY=/home/pi/librga/libs/Linux/gcc-aarch64/librga.so
```

`RGA_LIBRARY` is only needed when `librga.so` is not installed in the normal
system linker path.

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
  --device /dev/v4l/by-id/usb-MACROSILICON_USB3_Video_20210623-video-index0 \
  --audio-device hw:CARD=Video,DEV=0 \
  --audio-codec opus \
  --audio-frame-ms 20 \
  --width 1920 \
  --height 1080 \
  --fps 50 \
  --bitrate 18000000 \
  --gop 50 \
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

Measured on a NanoPi R3S LTS with RK3568, Rockchip MPP JPEG decode, Rockchip MPP
H.264 encode, Opus audio, RTSP/TCP, and one client.

The best tested direct RTSP configuration was:

```bash
rk-hdmi-streamer \
  --decoder mppjpeg \
  --v4l2-dmabuf \
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
enabled and no queue drops. With RTSP/TCP interleaving, `--rtp-payload 1400`
keeps RTP packets well below the protocol length limit while avoiding thousands
of small packetization, queue, and `sendmsg` operations per second. Use a smaller
payload only when a specific client or relay requires it.

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
- Use `--audio-frame-ms 20` for the normal Opus RTP frame size; valid values
  are `2.5`, `5`, `10`, `20`, `40`, and `60`.

If the service does not start:

```bash
journalctl -u rk-hdmi-streamer.service -n 100 --no-pager
```

## License

MIT.
