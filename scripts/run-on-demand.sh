#!/bin/sh
set -eu

PATH_NAME="${MTX_PATH:-capture}"
RTSP_PORT="${RTSP_PORT:-8554}"
STREAMER_BIN="${STREAMER_BIN:-/usr/local/bin/rk-hdmi-streamer}"
DEVICE="${DEVICE:-/dev/v4l/by-id/usb-MACROSILICON_USB3_Video_20210623-video-index0}"
AUDIO_DEVICE="${AUDIO_DEVICE:-hw:CARD=Video,DEV=0}"
AUDIO_CODEC="${AUDIO_CODEC:-opus}"
AUDIO_GAIN="${AUDIO_GAIN:-3.0}"
AUDIO_FRAME_MS="${AUDIO_FRAME_MS:-20}"
STREAM_PROFILE="${STREAM_PROFILE:-mjpeg}"
WIDTH="${WIDTH:-1920}"
HEIGHT="${HEIGHT:-1080}"
FPS="${FPS:-50}"
BITRATE="${BITRATE:-18000000}"
GOP="${GOP:-50}"
RTP_PAYLOAD="${RTP_PAYLOAD:-1400}"

exec "$STREAMER_BIN" \
  --stream-profile "$STREAM_PROFILE" \
  --device "$DEVICE" \
  --audio-device "$AUDIO_DEVICE" \
  --audio-codec "$AUDIO_CODEC" \
  --audio-gain "$AUDIO_GAIN" \
  --audio-frame-ms "$AUDIO_FRAME_MS" \
  --width "$WIDTH" \
  --height "$HEIGHT" \
  --fps "$FPS" \
  --bitrate "$BITRATE" \
  --gop "$GOP" \
  --rtp-payload "$RTP_PAYLOAD" \
  --output "rtsp://127.0.0.1:${RTSP_PORT}/${PATH_NAME}"
