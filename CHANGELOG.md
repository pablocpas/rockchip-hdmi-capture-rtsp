# Changelog

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
