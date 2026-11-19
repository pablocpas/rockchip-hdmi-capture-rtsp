# Rockchip HDMI Capture RTSP Streamer

Low-latency HDMI UVC capture and direct RTSP/TCP server for Rockchip SBCs.

Captures HDMI video (V4L2) and audio (ALSA) from MacroSilicon-style USB capture
dongles, processes video on Rockchip hardware (MPP / RGA), and serves one or
more RTSP/TCP streams directly without FFmpeg or an external relay.

## Features

- Single-thread `epoll` RTSP/TCP interleaved server with pooled RTP buffers, `TCP_NOTSENT_LOWAT` backpressure control, and per-stream send timeouts.
- Shared hardware capture feeding multiple simultaneous **on-demand** output streams (`/hq`, `/h265`, `/coche`, `/audio`): each encoder and RGA scaler is created only while clients are connected.
- Audio-only endpoints (`video=false`): never open V4L2, MPP, or RGA when no video stream is active.
- Rockchip MPP hardware encoding (H.264 and H.265/HEVC) with `vbr` and `cbr` rate control.
- Hardware RGA NV12 downscaling (`1080p -> 720p`) and FPS decimation (`50 -> 25 fps`), available under both `mjpeg` and `rga` capture modes.
- Shared ALSA audio capture (Opus or L16), encoded once and broadcast to all active streams.
- Composable Unix CLI (`--opt=val`, short flags, `k`/`M` bitrates, `-s WxH`, stdin config, `--check-config`, `--print-config`, `SIGUSR1`/`SIGUSR2`, manpage).

## Capture Modes

Select the capture pipeline in `[capture]` or with `--stream-profile`:

| Mode | Pipeline | When to use |
| --- | --- | --- |
| `mjpeg` | MJPEG DMA-BUF -> MPP JPEG -> NV12 | **Default.** Low USB/kernel load (~15 MB/s), works on USB2 and USB3, still uses RGA for 720p downscaling |
| `rga` | YUYV DMA-BUF -> RGA NV12 | No prior JPEG compression and ~2–3 ms lower latency on dedicated USB3 (207 MB/s @ 1080p50) |
| `yuyv-libyuv` | YUYV -> CPU libyuv/NEON NV12 | Fallback/debug when RGA is unavailable |

If a capture device rejects DMA-BUF in `mjpeg` mode, capture automatically falls
back to `mmap` while still decoding on MPP.

### Tested Hardware

- NanoPi R3S LTS (RK3568)
- MacroSilicon MS2131 (USB3): supports 1080p50/60 in `MJPG` and `YUYV` (`mjpeg` recommended for lowest USB/kernel load; `rga` for sharpest desktop/text capture)
- MacroSilicon MS2109 (USB2): use `mjpeg` at the highest mode listed by `v4l2-ctl --list-formats-ext`

## Quick Start From Release

Install runtime dependencies:

```bash
sudo apt update
sudo apt install -y v4l-utils alsa-utils libasound2 libopus0
```

Rockchip MPP runtime libraries must also be present (included in most Rockchip
board images).

Install from a release tarball:

```bash
tar -xzf rockchip-hdmi-capture-rtsp-*-linux-aarch64.tar.gz
cd rockchip-hdmi-capture-rtsp-*-linux-aarch64
sudo install -m 755 bin/rk-hdmi-streamer /usr/local/bin/rk-hdmi-streamer
sudo install -m 644 man/rk-hdmi-streamer.1 /usr/local/share/man/man1/rk-hdmi-streamer.1
sudo install -m 644 systemd/rk-hdmi-streamer.conf /etc/rk-hdmi-streamer.conf
sudo install -m 644 systemd/rk-hdmi-streamer-direct.service /etc/systemd/system/rk-hdmi-streamer.service
```

Validate configuration and start the service:

```bash
sudoedit /etc/rk-hdmi-streamer.conf
rk-hdmi-streamer -c /etc/rk-hdmi-streamer.conf --check-config
sudo systemctl daemon-reload
sudo systemctl enable --now rk-hdmi-streamer.service
```

Default endpoints:

```text
rtsp://BOARD_IP:8554/hq     # 1080p50 H.264 VBR 15M + Opus (LAN / Wi-Fi)
rtsp://BOARD_IP:8554/h265   # 1080p50 H.265 VBR 6M  + Opus (remote / 5G)
rtsp://BOARD_IP:8554/coche  # 720p25  H.265 VBR 2M  + Opus (mobile / car, 10 s timeout)
rtsp://BOARD_IP:8554/audio  # Opus audio-only (no video capture opened, 12 s timeout)
```

For guided configuration, run `sudo scripts/configure.sh`.

## Configuration

The systemd unit reads `/etc/rk-hdmi-streamer.conf`:

```ini
[capture]
device=/dev/v4l/by-id/usb-MACROSILICON_USB3_Video_20210623-video-index0
audio-device=hw:CARD=Video,DEV=0
audio=true
audio-codec=opus
audio-gain=3.0
audio-frame-ms=20
stream-profile=mjpeg
width=1920
height=1080
fps=50
video-codec=h264
rc-mode=vbr
bitrate=15000000
gop=50
rtp-payload=1400
listen-rtsp=:8554
rtsp-path=/hq
rtsp-send-timeout-ms=3000
max-clients=5
rtsp-debug=false

[stream.h265]
path=/h265
codec=h265
bitrate=6000000
rc-mode=vbr
gop=50
rtsp-send-timeout-ms=6000

[stream.coche]
path=/coche
codec=h265
width=1280
height=720
fps=25
bitrate=2000000
rc-mode=vbr
gop=25
rtsp-send-timeout-ms=10000

[stream.audio]
path=/audio
video=false
audio=true
rtsp-send-timeout-ms=12000
```

- Prefer `/dev/v4l/by-id/...video-index0` over `/dev/videoN` so device numbering stays stable across reboots.
- `[capture]` (legacy alias `[main]`) defines the shared HDMI/ALSA input and default stream.
- Each `[stream.<name>]` (legacy alias `[profile.<name>]`) inherits `[capture]`, overrides output parameters (`path`, `video`, `audio`, `codec`, `rc-mode`, `bitrate`, `size`/`width`/`height`, `fps`, `gop`, `rtsp-send-timeout-ms`), and runs on demand.
- On RK3568 (VEPU540), simultaneous streams such as `/hq` (`1080p50`) + `/coche` (`720p25`) + `/audio` fit comfortably within the hardware encoder budget.

## CLI & Signals

See `rk-hdmi-streamer --help` or `man rk-hdmi-streamer` ([man/rk-hdmi-streamer.1](man/rk-hdmi-streamer.1)) for the full option list.

- All long options accept `--opt value` and `--opt=value`; bitrates accept `k`/`M`/`G` suffixes (`15M`, `2.5M`); resolutions accept `-s 1920x1080`.
- `--check-config` validates and exits (`0` valid, `2` config/CLI error, `1` runtime error).
- `--print-config` prints effective normalized INI to `stdout`; `-c -` reads INI from `stdin`.
- `-o PATH` writes raw Annex-B video (`-` for `stdout`) or publishes to `rtsp://...`.
- Signals: `SIGINT`/`SIGTERM` stop cleanly, `SIGUSR1` prints live stats to `stderr`, `SIGUSR2` forces an immediate IDR keyframe on all active video encoders.

Example ad-hoc run:

```bash
rk-hdmi-streamer --stream-profile=mjpeg -s 1920x1080 -r 50 -b 15M --rc-mode=vbr \
  --audio-codec=opus -l :8554 --rtsp-path=/hq --max-clients=5 \
  --rtsp-stream=path=/coche,size=1280x720,fps=25,codec=h265,bitrate=2M,rc-mode=vbr,gop=25,send-timeout-ms=10000 \
  --rtsp-stream=path=/audio,video=false,audio=true,send-timeout-ms=12000
```

## RGA Runtime

RGA (`librga.so`) is optional and loaded dynamically via `dlopen`. It accelerates
both `YUYV -> NV12` conversion (`stream-profile=rga`) and hardware `NV12`
downscaling (e.g. `1080p -> 720p` in `/coche`, even under `stream-profile=mjpeg`).

Install `librga.so` system-wide:

```bash
sudo install -m 755 third_party/librga/aarch64/librga.so /usr/local/lib/librga.so
sudo ldconfig
```

or specify `rga-library=/path/to/librga.so` under `[capture]`. If `librga.so` is
absent on a downscaled stream, scaling falls back automatically to CPU.

## Build From Source

Install build dependencies (plus Rockchip MPP development headers, e.g. `librockchip-mpp-dev`):

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config git \
  libasound2-dev libopus-dev libyuv-dev
```

Build and install (recommended RK3568 configuration with RGA and Cortex-A55/LSE tuning):

```bash
git clone --depth=1 https://github.com/airockchip/librga.git ~/librga
git clone https://github.com/pablocpas/rockchip-hdmi-capture-rtsp.git
cd rockchip-hdmi-capture-rtsp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRK_CPU=cortex-a55 -DRGA_ROOT="$HOME/librga"
cmake --build build -j"$(nproc)"
sudo cmake --install build
```

Without RGA headers, omit `-DRGA_ROOT=...`.

### Release Tarball

```bash
RK_CPU=cortex-a55 RGA_ROOT="$HOME/librga" scripts/package-release.sh v0.3.0
```

Outputs `dist/rockchip-hdmi-capture-rtsp-v0.3.0-linux-aarch64.tar.gz` and its `.sha256`.

## Testing & Diagnostics

Host unit tests (no Rockchip hardware required):

```bash
cmake -S . -B build-host -DRK_BUILD_STREAMER=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-host --parallel
ctest --test-dir build-host --output-on-failure
```

Sustained loopback stress test (add `-DRK_STRESS_SECONDS=60` and run `ctest --test-dir build-host -L stress -V`).
CI runs the suite under ASan/UBSan; TSan can be run with `-DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer'`.

Live deployment test and board profiling:

```bash
tests/stress-live.sh rtsp://BOARD_IP:8554/hq 30 3 4
pidstat -ur -p "$(systemctl show -p MainPID --value rk-hdmi-streamer.service)" 1
mpstat 1 10
grep -E 'MemAvailable|CmaTotal|CmaFree' /proc/meminfo
```

Inspect a running setup:

```bash
rk-hdmi-streamer -c /etc/rk-hdmi-streamer.conf --print-config
v4l2-ctl --list-devices
arecord -l
ss -ltn 'sport = :8554'
journalctl -u rk-hdmi-streamer.service -n 100 --no-pager
```

## Network & Troubleshooting

- The RTSP server has no authentication; bind to a trusted interface or expose it over a VPN such as WireGuard.
- WireGuard + RTSP/TCP interleaved works well over mobile connections (transparent IP roaming across cell handovers, no UDP-over-UDP loss amplification).
- Recommended Linux TCP sysctls:

```bash
sudo tee /etc/sysctl.d/99-rk-streaming.conf >/dev/null <<'EOF'
net.ipv4.tcp_congestion_control=bbr
net.core.default_qdisc=fq
net.ipv4.tcp_slow_start_after_idle=0
EOF
sudo sysctl --system
```

- **Black video or artifacts:** verify the mode with `v4l2-ctl -d <dev> --list-formats-ext`, test `stream-profile=mjpeg`, and check `journalctl -u rk-hdmi-streamer.service`.
- **Missing audio:** verify `arecord -l` and `audio-device`; on VLC for Android, switch audio output from `AudioTrack` to `OpenSL ES` if audio stops on reconnect.

## License

MIT.
