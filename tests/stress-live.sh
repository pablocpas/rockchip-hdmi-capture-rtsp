#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 4 ]; then
  printf 'Usage: %s RTSP_URL [seconds_per_round=15] [clients=3] [rounds=3]\n' "$0" >&2
  exit 2
fi

url="$1"
seconds="${2:-15}"
clients="${3:-3}"
rounds="${4:-3}"
startup_slack=10

check_count() {
  case "$1" in
    ''|0*|*[!0-9]*) return 1 ;;
  esac
  [ "${#1}" -le "${#2}" ] && [ "$1" -le "$2" ]
}

if ! check_count "$seconds" 3600 || ! check_count "$clients" 16 || ! check_count "$rounds" 1000; then
  printf 'Expected positive counts, up to 3600 seconds, 16 clients and 1000 rounds.\n' >&2
  exit 2
fi
for utility in ffmpeg timeout; do
  if ! command -v "$utility" >/dev/null 2>&1; then
    printf '%s is required\n' "$utility" >&2
    exit 2
  fi
done

work="$(mktemp -d)"
jobs=""
trap 'for pid in $jobs; do kill "$pid" 2>/dev/null || :; done; rm -rf -- "$work"' 0
trap 'exit 1' HUP INT TERM
round=1
while [ "$round" -le "$rounds" ]; do
  client=1
  while [ "$client" -le "$clients" ]; do
    timeout --kill-after=2 "$((seconds + startup_slack))" ffmpeg -nostdin -hide_banner       -loglevel error -nostats -xerror -rtsp_transport tcp -i "$url"       -map 0:v:0 -map '0:a?' -t "$seconds"       -progress "$work/progress.$client" -f null -       > /dev/null 2>"$work/ffmpeg.$client" &
    jobs="$jobs $!"
    client=$((client + 1))
  done

  failed=0
  client=1
  for pid in $jobs; do
    if wait "$pid"; then
      if ! awk -F= '
        $1 == "frame" { frames = $2 + 0 }
        $1 == "progress" { progress = $2 }
        END { if (frames > 0 && progress == "end") print frames; else exit 1 }
      ' "$work/progress.$client" >"$work/frames.$client"; then
        printf 'round=%s client=%s did not finish decoding any video\n' "$round" "$client" >&2
        failed=1
      else
        printf 'round=%s client=%s decoded_frames=%s\n' "$round" "$client" "$(cat "$work/frames.$client")"
      fi
    else
      status=$?
      printf 'round=%s client=%s exit=%s\n' "$round" "$client" "$status" >&2
      cat "$work/ffmpeg.$client" >&2
      failed=1
    fi
    client=$((client + 1))
  done
  jobs=""
  [ "$failed" -eq 0 ] || exit 1
  round=$((round + 1))
done
