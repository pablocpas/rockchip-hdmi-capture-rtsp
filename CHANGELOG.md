# Changelog

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
