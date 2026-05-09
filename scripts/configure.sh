#!/bin/sh
set -u

CONFIG_FILE="${CONFIG_FILE:-/etc/default/rk-hdmi-streamer}"
SERVICE_NAME="${SERVICE_NAME:-rk-hdmi-streamer.service}"

DEFAULT_DEVICE="/dev/v4l/by-id/usb-MACROSILICON_USB3_Video_20210623-video-index0"
DEFAULT_AUDIO_DEVICE="hw:CARD=Video,DEV=0"

say() {
  printf '%s\n' "$*"
}

ask() {
  prompt="$1"
  default="$2"
  printf '%s [%s]: ' "$prompt" "$default" >&2
  read -r value
  if [ -z "$value" ]; then
    printf '%s' "$default"
  else
    printf '%s' "$value"
  fi
}

confirm() {
  prompt="$1"
  default="${2:-Y}"
  if [ "$default" = "Y" ]; then
    printf '%s [Y/n]: ' "$prompt" >&2
  else
    printf '%s [y/N]: ' "$prompt" >&2
  fi
  read -r value
  case "$value" in
    y|Y|yes|YES) return 0 ;;
    n|N|no|NO) return 1 ;;
    "") [ "$default" = "Y" ] ;;
    *) return 1 ;;
  esac
}

have_cmd() {
  command -v "$1" >/dev/null 2>&1
}

video_capture_node() {
  dev="$1"
  have_cmd v4l2-ctl || return 1
  v4l2-ctl -d "$dev" --all 2>/dev/null | grep -q "Format Video Capture:"
}

list_candidate_video_devices() {
  seen=""
  for dev in /dev/v4l/by-id/*MACROSILICON*video-index0 /dev/v4l/by-id/*video-index0 /dev/video*; do
    [ -e "$dev" ] || continue
    real="$(readlink -f "$dev" 2>/dev/null || printf '%s' "$dev")"
    case " $seen " in
      *" $real "*) continue ;;
    esac
    seen="$seen $real"
    if video_capture_node "$dev"; then
      printf '%s\n' "$dev"
    fi
  done
}

device_supports_mode() {
  dev="$1"
  pix="$2"
  width="$3"
  height="$4"
  fps="$5"
  size="${width}x${height}"

  have_cmd v4l2-ctl || return 1
  v4l2-ctl -d "$dev" --list-formats-ext 2>/dev/null | awk \
    -v pix="$pix" \
    -v size="$size" \
    -v fps="$fps" '
      $0 ~ "\\[[0-9]+\\]: '\''" pix "'\''" {
        in_fmt = 1
        in_size = 0
        next
      }
      $0 ~ "\\[[0-9]+\\]: '\''" && $0 !~ "\\[[0-9]+\\]: '\''" pix "'\''" {
        in_fmt = 0
        in_size = 0
        next
      }
      in_fmt && index($0, "Size: Discrete " size) {
        in_size = 1
        next
      }
      in_fmt && /Size: Discrete/ && index($0, "Size: Discrete " size) == 0 {
        in_size = 0
        next
      }
      in_fmt && in_size && $0 ~ "\\(" fps "\\.000 fps\\)" {
        found = 1
      }
      END {
        exit(found ? 0 : 1)
      }
    '
}

mode_bitrate() {
  width="$1"
  height="$2"
  fps="$3"
  if [ "$width" -ge 1920 ] && [ "$fps" -ge 50 ]; then
    printf '18000000'
  elif [ "$width" -ge 1920 ]; then
    printf '12000000'
  elif [ "$fps" -ge 50 ]; then
    printf '8000000'
  else
    printf '6000000'
  fi
}

set_mode() {
  STREAM_PROFILE="$1"
  WIDTH="$2"
  HEIGHT="$3"
  FPS="$4"
  BITRATE="$(mode_bitrate "$WIDTH" "$HEIGHT" "$FPS")"
  GOP="$FPS"
}

try_common_modes() {
  dev="$1"
  pix="$2"
  # Preference order: highest useful resolution first, then cadence. This is not
  # TV-specific; 60/50/30 are common HDMI capture modes across cameras, consoles,
  # desktops, and set-top boxes.
  for mode in \
    "1920 1080 60" \
    "1920 1080 50" \
    "1920 1080 30" \
    "1280 720 60" \
    "1280 720 50" \
    "1280 720 30" \
    "640 480 60" \
    "640 480 30"
  do
    set -- $mode
    if device_supports_mode "$dev" "$pix" "$1" "$2" "$3"; then
      printf '%s %s %s\n' "$1" "$2" "$3"
      return 0
    fi
  done
  return 1
}

rga_available() {
  [ -n "${RGA_LIBRARY:-}" ] && [ -f "$RGA_LIBRARY" ] && return 0
  [ -f /usr/local/lib/librga.so ] && return 0
  [ -f /usr/lib/librga.so ] && return 0
  [ -f /usr/lib/aarch64-linux-gnu/librga.so ] && return 0
  [ -f "$HOME/librga/libs/Linux/gcc-aarch64/librga.so" ] && return 0
  [ -f /home/pi/librga/libs/Linux/gcc-aarch64/librga.so ] && return 0
  have_cmd ldconfig && ldconfig -p 2>/dev/null | grep -q 'librga\.so' && return 0
  return 1
}

detect_rga_library() {
  [ -n "${RGA_LIBRARY:-}" ] && [ -f "$RGA_LIBRARY" ] && {
    printf '%s' "$RGA_LIBRARY"
    return
  }
  for path in \
    /usr/local/lib/librga.so \
    /usr/lib/librga.so \
    /usr/lib/aarch64-linux-gnu/librga.so \
    "$HOME/librga/libs/Linux/gcc-aarch64/librga.so" \
    /home/pi/librga/libs/Linux/gcc-aarch64/librga.so
  do
    [ -f "$path" ] && {
      printf '%s' "$path"
      return
    }
  done
}

detect_audio_device() {
  if have_cmd arecord && arecord -l 2>/dev/null | grep -q "card [0-9][0-9]*: Video "; then
    printf '%s' "$DEFAULT_AUDIO_DEVICE"
    return
  fi
  printf '%s' "$DEFAULT_AUDIO_DEVICE"
}

first_video_device() {
  candidates="$(list_candidate_video_devices)"
  first="$(printf '%s\n' "$candidates" | sed -n '1p')"
  if [ -n "$first" ]; then
    printf '%s' "$first"
  else
    printf '%s' "$DEFAULT_DEVICE"
  fi
}

configure_auto() {
  dev="$1"
  if rga_available; then
    if device_supports_mode "$dev" YUYV "$WIDTH" "$HEIGHT" "$FPS"; then
      set_mode rga "$WIDTH" "$HEIGHT" "$FPS"
      RGA_LIBRARY="$(detect_rga_library)"
      return
    fi
    mode="$(try_common_modes "$dev" YUYV || true)"
    if [ -n "$mode" ]; then
      set -- $mode
      set_mode rga "$1" "$2" "$3"
      RGA_LIBRARY="$(detect_rga_library)"
      return
    fi
  fi

  if device_supports_mode "$dev" MJPG "$WIDTH" "$HEIGHT" "$FPS"; then
    set_mode mjpeg "$WIDTH" "$HEIGHT" "$FPS"
    return
  fi

  mode="$(try_common_modes "$dev" MJPG || true)"
  if [ -n "$mode" ]; then
    set -- $mode
    set_mode mjpeg "$1" "$2" "$3"
    return
  fi

  set_mode mjpeg 1920 1080 30
}

configure_rga() {
  dev="$1"
  mode="$(try_common_modes "$dev" YUYV || true)"
  if [ -n "$mode" ]; then
    set -- $mode
    set_mode rga "$1" "$2" "$3"
  else
    set_mode rga 1920 1080 30
  fi
  RGA_LIBRARY="$(detect_rga_library)"
}

configure_mjpeg() {
  dev="$1"
  mode="$(try_common_modes "$dev" MJPG || true)"
  if [ -n "$mode" ]; then
    set -- $mode
    set_mode mjpeg "$1" "$2" "$3"
  else
    set_mode mjpeg 1920 1080 30
  fi
}

configure_tv50() {
  dev="$1"
  if rga_available && device_supports_mode "$dev" YUYV 1920 1080 50; then
    set_mode rga 1920 1080 50
    RGA_LIBRARY="$(detect_rga_library)"
  elif device_supports_mode "$dev" MJPG 1920 1080 50; then
    set_mode mjpeg 1920 1080 50
  elif rga_available && device_supports_mode "$dev" YUYV 1280 720 50; then
    set_mode rga 1280 720 50
    RGA_LIBRARY="$(detect_rga_library)"
  elif device_supports_mode "$dev" MJPG 1280 720 50; then
    set_mode mjpeg 1280 720 50
  else
    set_mode mjpeg 1920 1080 30
  fi
}

configure_custom() {
  STREAM_PROFILE="$(ask "STREAM_PROFILE" "${STREAM_PROFILE:-mjpeg}")"
  WIDTH="$(ask "WIDTH" "${WIDTH:-1920}")"
  HEIGHT="$(ask "HEIGHT" "${HEIGHT:-1080}")"
  FPS="$(ask "FPS" "${FPS:-30}")"
  BITRATE="$(ask "BITRATE" "${BITRATE:-$(mode_bitrate "$WIDTH" "$HEIGHT" "$FPS")}")"
  GOP="$(ask "GOP" "${GOP:-$FPS}")"
  if [ "$STREAM_PROFILE" = "rga" ]; then
    RGA_LIBRARY="$(ask "RGA_LIBRARY, empty if installed system-wide" "${RGA_LIBRARY:-$(detect_rga_library)}")"
  fi
}

write_config() {
  out="$1"
  tmp="$(mktemp)"
  cat >"$tmp" <<EOF
# Defaults for rk-hdmi-streamer.service.
# Generated by scripts/configure.sh.

DEVICE=$DEVICE
AUDIO_DEVICE=$AUDIO_DEVICE
AUDIO_CODEC=$AUDIO_CODEC
AUDIO_GAIN=$AUDIO_GAIN
AUDIO_FRAME_MS=$AUDIO_FRAME_MS
STREAM_PROFILE=$STREAM_PROFILE
# Valid profiles:
#   rga         - recommended when YUYV mode and Rockchip RGA are available
#   mjpeg       - safest fallback, UVC MJPEG -> MPP JPEG -> MPP H.264
#   yuyv-libyuv - debug/fallback, YUYV conversion on CPU
EOF
  if [ -n "${RGA_LIBRARY:-}" ]; then
    printf 'RGA_LIBRARY=%s\n' "$RGA_LIBRARY" >>"$tmp"
  else
    printf '# RGA_LIBRARY=/home/pi/librga/libs/Linux/gcc-aarch64/librga.so\n' >>"$tmp"
  fi
  cat >>"$tmp" <<EOF
WIDTH=$WIDTH
HEIGHT=$HEIGHT
FPS=$FPS
BITRATE=$BITRATE
GOP=$GOP
RTP_PAYLOAD=$RTP_PAYLOAD
CPU_GOVERNOR=$CPU_GOVERNOR
LISTEN_RTSP=$LISTEN_RTSP
RTSP_PATH=$RTSP_PATH
MAX_CLIENTS=$MAX_CLIENTS
RTSP_DEBUG=$RTSP_DEBUG
EOF

  if [ "$(id -u)" -eq 0 ] || [ -w "$(dirname "$out")" ]; then
    install -m 644 "$tmp" "$out"
    rm -f "$tmp"
    CONFIG_WRITTEN=1
    say "Wrote $out"
  else
    fallback="./rk-hdmi-streamer.env.generated"
    install -m 644 "$tmp" "$fallback"
    rm -f "$tmp"
    CONFIG_WRITTEN=0
    say "Wrote $fallback"
    say "Install it with:"
    say "  sudo install -m 644 $fallback $out"
  fi
}

restart_service() {
  if ! have_cmd systemctl; then
    say "systemctl not found; restart the service manually."
    return
  fi

  if [ "$(id -u)" -eq 0 ]; then
    systemctl daemon-reload
    systemctl restart "$SERVICE_NAME"
  else
    sudo systemctl daemon-reload
    sudo systemctl restart "$SERVICE_NAME"
  fi
  say "Restarted $SERVICE_NAME"
}

# Start with existing config when present so the script works well for changing
# a running installation, not only for first setup.
if [ -r "$CONFIG_FILE" ]; then
  # shellcheck disable=SC1090
  . "$CONFIG_FILE"
fi

DEVICE="${DEVICE:-$(first_video_device)}"
AUDIO_DEVICE="${AUDIO_DEVICE:-$(detect_audio_device)}"
AUDIO_CODEC="${AUDIO_CODEC:-opus}"
AUDIO_GAIN="${AUDIO_GAIN:-3.0}"
AUDIO_FRAME_MS="${AUDIO_FRAME_MS:-20}"
STREAM_PROFILE="${STREAM_PROFILE:-mjpeg}"
WIDTH="${WIDTH:-1920}"
HEIGHT="${HEIGHT:-1080}"
FPS="${FPS:-30}"
BITRATE="${BITRATE:-$(mode_bitrate "$WIDTH" "$HEIGHT" "$FPS")}"
GOP="${GOP:-$FPS}"
RTP_PAYLOAD="${RTP_PAYLOAD:-1400}"
CPU_GOVERNOR="${CPU_GOVERNOR:-performance}"
LISTEN_RTSP="${LISTEN_RTSP:-:8554}"
RTSP_PATH="${RTSP_PATH:-/capture}"
MAX_CLIENTS="${MAX_CLIENTS:-3}"
RTSP_DEBUG="${RTSP_DEBUG:-0}"
CONFIG_WRITTEN=0

say "rk-hdmi-streamer configuration"
say "Config file: $CONFIG_FILE"
say
say "Detected video capture devices:"
candidates="$(list_candidate_video_devices)"
if [ -n "$candidates" ]; then
  printf '%s\n' "$candidates" | nl -w2 -s') '
else
  say "  none detected"
fi
say

DEVICE="$(ask "Video device" "$DEVICE")"
AUDIO_DEVICE="$(ask "Audio device" "$AUDIO_DEVICE")"

say
say "Choose a configuration profile:"
say "  1) Auto: prefer RGA/YUYV when available, otherwise MJPEG fallback"
say "  2) Best performance: RGA/YUYV, choose the best advertised common mode"
say "  3) Most compatible: MJPEG, choose the best advertised common mode"
say "  4) TV cadence: prefer 1080p50, fallback to 720p50 or 1080p30"
say "  5) Custom"
printf 'Profile [1]: '
read -r choice
[ -n "$choice" ] || choice=1

case "$choice" in
  1) configure_auto "$DEVICE" ;;
  2) configure_rga "$DEVICE" ;;
  3) configure_mjpeg "$DEVICE" ;;
  4) configure_tv50 "$DEVICE" ;;
  5) configure_custom ;;
  *) say "Unknown profile: $choice"; exit 1 ;;
esac

if [ "$STREAM_PROFILE" = "rga" ]; then
  if ! device_supports_mode "$DEVICE" YUYV "$WIDTH" "$HEIGHT" "$FPS"; then
    say "Warning: selected RGA/YUYV but the device does not advertise YUYV ${WIDTH}x${HEIGHT}@${FPS}."
  fi
  if ! rga_available && [ -z "${RGA_LIBRARY:-}" ]; then
    say "Warning: RGA was selected but librga.so was not found. Install librga or set RGA_LIBRARY."
  fi
fi
if [ "$STREAM_PROFILE" = "mjpeg" ] && ! device_supports_mode "$DEVICE" MJPG "$WIDTH" "$HEIGHT" "$FPS"; then
  say "Warning: selected MJPEG but the device does not advertise MJPG ${WIDTH}x${HEIGHT}@${FPS}."
fi

AUDIO_CODEC="$(ask "Audio codec" "$AUDIO_CODEC")"
AUDIO_GAIN="$(ask "Audio gain" "$AUDIO_GAIN")"
AUDIO_FRAME_MS="$(ask "Audio frame ms" "$AUDIO_FRAME_MS")"
MAX_CLIENTS="$(ask "Max RTSP clients" "$MAX_CLIENTS")"
LISTEN_RTSP="$(ask "RTSP listen address" "$LISTEN_RTSP")"
RTSP_PATH="$(ask "RTSP path" "$RTSP_PATH")"

say
say "Configuration summary:"
say "  DEVICE=$DEVICE"
say "  AUDIO_DEVICE=$AUDIO_DEVICE"
say "  STREAM_PROFILE=$STREAM_PROFILE"
[ -n "${RGA_LIBRARY:-}" ] && say "  RGA_LIBRARY=$RGA_LIBRARY"
say "  WIDTH=$WIDTH HEIGHT=$HEIGHT FPS=$FPS BITRATE=$BITRATE GOP=$GOP"
say "  RTSP URL: rtsp://BOARD_IP:${LISTEN_RTSP##*:}$RTSP_PATH"
say
if ! confirm "Write configuration to $CONFIG_FILE?" Y; then
  say "Not written."
  exit 0
fi

write_config "$CONFIG_FILE"

if [ "$CONFIG_WRITTEN" -eq 1 ] && confirm "Restart $SERVICE_NAME now?" Y; then
  restart_service
fi

say
say "Validate with:"
say "  scripts/doctor.sh"
