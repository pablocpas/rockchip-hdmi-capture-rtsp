#!/bin/sh
set -u

CONFIG_FILE="${CONFIG_FILE:-/etc/default/rk-hdmi-streamer}"

DEVICE="${DEVICE:-/dev/v4l/by-id/usb-MACROSILICON_USB3_Video_20210623-video-index0}"
AUDIO_DEVICE="${AUDIO_DEVICE:-hw:CARD=Video,DEV=0}"
VIDEO_CODEC="${VIDEO_CODEC:-h264}"
AUDIO_CODEC="${AUDIO_CODEC:-opus}"
STREAM_PROFILE="${STREAM_PROFILE:-mjpeg}"
WIDTH="${WIDTH:-1920}"
HEIGHT="${HEIGHT:-1080}"
FPS="${FPS:-50}"
BITRATE="${BITRATE:-18000000}"
GOP="${GOP:-50}"
LISTEN_RTSP="${LISTEN_RTSP:-:8554}"
RTSP_PATH="${RTSP_PATH:-/capture}"
RGA_LIBRARY="${RGA_LIBRARY:-}"

if [ -r "$CONFIG_FILE" ]; then
  # shellcheck disable=SC1090
  . "$CONFIG_FILE"
fi

PASS=0
WARN=0
FAIL=0

say() {
  printf '%s\n' "$*"
}

section() {
  printf '\n== %s ==\n' "$*"
}

ok() {
  PASS=$((PASS + 1))
  printf '[OK] %s\n' "$*"
}

warn() {
  WARN=$((WARN + 1))
  printf '[WARN] %s\n' "$*"
}

fail() {
  FAIL=$((FAIL + 1))
  printf '[FAIL] %s\n' "$*"
}

have_cmd() {
  command -v "$1" >/dev/null 2>&1
}

need_cmd() {
  if have_cmd "$1"; then
    ok "$1 found: $(command -v "$1")"
    return 0
  fi
  fail "$1 not found"
  return 1
}

profile_pixfmt() {
  case "$STREAM_PROFILE" in
    mjpeg) printf 'MJPG' ;;
    rga|yuyv-libyuv|libyuv) printf 'YUYV' ;;
    *) printf 'MJPG' ;;
  esac
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

device_supports_mode() {
  dev="$1"
  pix="$2"
  width="$3"
  height="$4"
  fps="$5"
  size="${width}x${height}"

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

video_capture_node() {
  dev="$1"
  v4l2-ctl -d "$dev" --all 2>/dev/null | grep -q "Format Video Capture:"
}

list_candidate_video_devices() {
  seen=""
  for dev in /dev/v4l/by-id/*video-index0 /dev/video*; do
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

max_mode_hint() {
  dev="$1"

  if device_supports_mode "$dev" YUYV "$WIDTH" "$HEIGHT" "$FPS"; then
    if rga_available; then
      say "Recommended: STREAM_PROFILE=rga is available for the configured ${WIDTH}x${HEIGHT}@${FPS} mode."
    else
      say "Recommended: this mode supports YUYV, so STREAM_PROFILE=rga is the best-performance path once librga.so is installed."
    fi
    say "Fallback: STREAM_PROFILE=mjpeg remains the safest compatibility mode when advertised by the dongle."
    return
  fi

  if device_supports_mode "$dev" MJPG "$WIDTH" "$HEIGHT" "$FPS"; then
    say "Recommended: configured MJPEG mode is supported. Use STREAM_PROFILE=mjpeg unless a matching YUYV/RGA mode is available."
    return
  fi

  if device_supports_mode "$dev" YUYV 1920 1080 60 || device_supports_mode "$dev" YUYV 1920 1080 50 || device_supports_mode "$dev" YUYV 1920 1080 30; then
    say "Recommended: device has 1080p YUYV modes; use scripts/configure.sh and choose the RGA/YUYV profile."
    return
  fi

  if device_supports_mode "$dev" MJPG 1920 1080 60 || device_supports_mode "$dev" MJPG 1920 1080 50 || device_supports_mode "$dev" MJPG 1920 1080 30; then
    say "Recommended: device has 1080p MJPEG modes; use scripts/configure.sh and choose the MJPEG compatibility profile."
    return
  fi

  warn "No common 1080p/720p mode detected. Inspect formats manually."
}

section "Configuration"
say "Config file: $CONFIG_FILE"
say "DEVICE=$DEVICE"
say "AUDIO_DEVICE=$AUDIO_DEVICE"
say "VIDEO_CODEC=$VIDEO_CODEC"
say "AUDIO_CODEC=$AUDIO_CODEC"
say "STREAM_PROFILE=$STREAM_PROFILE"
say "WIDTH=$WIDTH"
say "HEIGHT=$HEIGHT"
say "FPS=$FPS"
say "BITRATE=$BITRATE"
say "GOP=$GOP"
say "LISTEN_RTSP=$LISTEN_RTSP"
say "RTSP_PATH=$RTSP_PATH"
[ -n "$RGA_LIBRARY" ] && say "RGA_LIBRARY=$RGA_LIBRARY"

section "Required Tools"
need_cmd v4l2-ctl || true
need_cmd arecord || true
if have_cmd rk-hdmi-streamer; then
  ok "rk-hdmi-streamer found: $(command -v rk-hdmi-streamer)"
else
  warn "rk-hdmi-streamer is not in PATH; install it to /usr/local/bin or run from build/"
fi
if have_cmd pkg-config && pkg-config --exists rockchip_mpp 2>/dev/null; then
  ok "Rockchip MPP pkg-config found: $(pkg-config --modversion rockchip_mpp 2>/dev/null)"
else
  warn "Rockchip MPP pkg-config was not found; release binaries only need runtime libraries"
fi
if rga_available; then
  ok "Rockchip RGA runtime found"
else
  warn "Rockchip RGA runtime not found; STREAM_PROFILE=rga will need librga.so or RGA_LIBRARY"
fi

section "Video Capture"
if ! have_cmd v4l2-ctl; then
  fail "Cannot inspect video devices without v4l2-ctl"
else
  candidates="$(list_candidate_video_devices)"
  if [ -z "$candidates" ]; then
    fail "No V4L2 video capture nodes found"
  else
    ok "Detected video capture node(s):"
    printf '%s\n' "$candidates" | sed 's/^/  /'
  fi

  if [ -e "$DEVICE" ]; then
    ok "Configured DEVICE exists"
  else
    fail "Configured DEVICE does not exist: $DEVICE"
    first_candidate="$(printf '%s\n' "$candidates" | sed -n '1p')"
    if [ -n "$first_candidate" ]; then
      say "Suggested DEVICE=$first_candidate"
    fi
  fi

  if [ -e "$DEVICE" ]; then
    if video_capture_node "$DEVICE"; then
      ok "Configured DEVICE is a Video Capture node"
    else
      fail "Configured DEVICE is not a Video Capture node; avoid metadata nodes such as video-index1"
    fi

    requested_pix="$(profile_pixfmt)"
    if device_supports_mode "$DEVICE" "$requested_pix" "$WIDTH" "$HEIGHT" "$FPS"; then
      ok "Requested mode exists: $requested_pix ${WIDTH}x${HEIGHT}@${FPS}"
    else
      fail "Requested mode was not found: $requested_pix ${WIDTH}x${HEIGHT}@${FPS}"
      say "Run:"
      say "  v4l2-ctl -d $DEVICE --list-formats-ext"
    fi

    max_mode_hint "$DEVICE"
  fi
fi

section "Audio Capture"
if ! have_cmd arecord; then
  fail "Cannot inspect audio devices without arecord"
else
  audio_cards="$(arecord -l 2>/dev/null || true)"
  if [ -z "$audio_cards" ]; then
    fail "No ALSA capture devices found"
  else
    ok "ALSA capture devices found"
    printf '%s\n' "$audio_cards" | sed 's/^/  /'
  fi

  case "$AUDIO_DEVICE" in
    hw:CARD=*,DEV=*)
      card_name="$(printf '%s' "$AUDIO_DEVICE" | sed -n 's/^hw:CARD=\([^,]*\),DEV=.*/\1/p')"
      if printf '%s\n' "$audio_cards" | grep -q "card [0-9][0-9]*: $card_name "; then
        ok "Configured AUDIO_DEVICE card is present: $card_name"
      else
        warn "Configured AUDIO_DEVICE card was not found by name: $AUDIO_DEVICE"
      fi
      ;;
    *)
      warn "AUDIO_DEVICE uses a custom ALSA name; verify it with arecord -L: $AUDIO_DEVICE"
      ;;
  esac
fi

section "RTSP Service"
port="$(printf '%s' "$LISTEN_RTSP" | sed 's/^.*://')"
if have_cmd ss; then
  if ss -ltn 2>/dev/null | grep -q "[:.]$port[[:space:]]"; then
    warn "TCP port $port is already listening. This is OK if rk-hdmi-streamer is running."
  else
    ok "TCP port $port is currently free"
  fi
else
  warn "ss not found; cannot check TCP port $port"
fi

if have_cmd systemctl; then
  if systemctl is-active --quiet rk-hdmi-streamer.service 2>/dev/null; then
    ok "rk-hdmi-streamer.service is active"
  else
    warn "rk-hdmi-streamer.service is not active"
  fi
  if systemctl is-active --quiet mediamtx.service 2>/dev/null; then
    warn "mediamtx.service is active; it may conflict with direct RTSP on port $port"
  else
    ok "mediamtx.service is not active"
  fi
fi

section "Configured Direct Command"
say "rk-hdmi-streamer \\"
say "  --stream-profile $STREAM_PROFILE \\"
say "  --device $DEVICE \\"
say "  --video-codec $VIDEO_CODEC \\"
say "  --audio-device $AUDIO_DEVICE \\"
say "  --audio-codec $AUDIO_CODEC \\"
say "  --width $WIDTH --height $HEIGHT --fps $FPS \\"
say "  --bitrate $BITRATE --gop $GOP \\"
say "  --listen-rtsp $LISTEN_RTSP --rtsp-path $RTSP_PATH"

if [ "$STREAM_PROFILE" != "rga" ] &&
   [ -e "$DEVICE" ] &&
   device_supports_mode "$DEVICE" YUYV "$WIDTH" "$HEIGHT" "$FPS" &&
   rga_available; then
  say
  say "Performance alternative for this same mode:"
  say "  sudo scripts/configure.sh"
  say "  Choose: Auto or Best performance (RGA/YUYV)"
fi

section "Summary"
say "OK=$PASS WARN=$WARN FAIL=$FAIL"
if [ "$FAIL" -gt 0 ]; then
  exit 1
fi
exit 0
