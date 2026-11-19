# Changelog

## Unreleased

- Split configuration, ALSA capture, video capture and RTSP into modules linked
  into the existing streamer executable.
- Added `--check-config` and `--print-config`. The optional configurator
  preserves unrelated INI settings and validates before writing when the new
  binary is installed.
- Replaced the diagnostic script with the configuration commands and standard
  Linux device, socket and log tools.
- Isolated RTSP playback state by capture generation and added retry handling
  for capture interruptions and failed audio opens.
- Removed system-wide CPU governor changes and the MediaMTX conflict from the
  systemd unit. The service restarts on failure.
- Resynchronize connected video clients when capture recovers, including clients
  waiting for a keyframe larger than the normal queue budget.
- Batch RTP writes within each frame and share the payload owner across its
  fragments. Allocate the MJPEG copy buffer only when using memory-mapped
  capture.
- Removed the optional CPU-based JPEG decoder and its build and runtime
  dependency. MJPEG decoding uses Rockchip MPP, including with `mmap`.
- Release the MPP encoder's temporary header buffer before streaming frames.
- Reuse RGA buffer descriptors between frames and free unused capture DMA-BUFs
  when switching to mmap.
- Reuse one MPP JPEG input packet per V4L2 DMA-BUF.
- Added native tests for configuration, the configurator and RTSP/RTP loopback.
  Added optional sustained RTSP tests and a multi-client FFmpeg test.

## v0.2.2

- Added experimental `--video-codec h265` direct RTSP support using Rockchip
  MPP HEVC encoding and RTP/H.265 packetization.
- Hardened H.265 RTP packetization for VLC compatibility.
- Added configurable video color range to avoid washed-out black levels.
- Added explicit INI-based RTSP profiles and an MPP JPEG decoder frame pool.
- Improved direct RTSP behavior for slow/mobile clients with frame-aware per-client
  queues, bounded backlog, non-blocking TCP writes, and keyframe resync after drops.
- Kept H.264 as the default video codec.

## v0.2.0

- Added interactive `scripts/configure.sh` for plug-and-play setup and service
  restart after configuration changes.
- Added `scripts/doctor.sh` to diagnose MacroSilicon V4L2 capture devices, ALSA
  audio, requested modes, RGA availability, RTSP port state, and systemd status.
- Added clearer MS2131/MS2109 guidance, including RGA/YUYV as the recommended
  performance path when available and MJPEG as the safest fallback.
- Added optional Rockchip RGA runtime packaging support.
- Added `mjpeg`, `rga`, and `yuyv-libyuv` stream profiles.
- Improved direct RTSP debug logging.
- Improved RTCP sender reports with compound `SR + SDES` packets and shared
  CNAME per RTSP session.
- Optimized direct RTSP packetization and packet lifetime handling.
- Added notes for VLC Android audio output issues.

## v0.1.0

Initial public release.

- 1080p30 UVC MJPEG capture path.
- Rockchip MPP JPEG decode and H.264 encode.
- Optional V4L2 DMABUF capture into MPP/DRM buffers.
- Direct RTSP/TCP server with H.264 video and Opus/L16 audio.
- Multi-client support for small deployments.
- Capture starts only while RTSP clients are playing.
- MediaMTX publisher compatibility mode.
- systemd service templates and `/etc/default/rk-hdmi-streamer` configuration.
- Release packaging script for Linux `aarch64` binaries.
