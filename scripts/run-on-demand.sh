#!/bin/sh
set -eu

PATH_NAME="${MTX_PATH:-capture}"
RTSP_PORT="${RTSP_PORT:-8554}"
STREAMER_BIN="${STREAMER_BIN:-/usr/local/bin/rk-hdmi-streamer}"
DEVICE="${DEVICE:-/dev/video0}"
AUDIO_DEVICE="${AUDIO_DEVICE:-hw:CARD=MS2109,DEV=0}"
AUDIO_CODEC="${AUDIO_CODEC:-opus}"
AUDIO_GAIN="${AUDIO_GAIN:-3.0}"
WIDTH="${WIDTH:-1920}"
HEIGHT="${HEIGHT:-1080}"
FPS="${FPS:-30}"
BITRATE="${BITRATE:-12000000}"
GOP="${GOP:-30}"
RTP_PAYLOAD="${RTP_PAYLOAD:-12000}"

exec "$STREAMER_BIN" \
  --decoder mppjpeg \
  --v4l2-dmabuf \
  --device "$DEVICE" \
  --audio-device "$AUDIO_DEVICE" \
  --audio-codec "$AUDIO_CODEC" \
  --audio-gain "$AUDIO_GAIN" \
  --width "$WIDTH" \
  --height "$HEIGHT" \
  --fps "$FPS" \
  --bitrate "$BITRATE" \
  --gop "$GOP" \
  --rtp-payload "$RTP_PAYLOAD" \
  --output "rtsp://127.0.0.1:${RTSP_PORT}/${PATH_NAME}"
