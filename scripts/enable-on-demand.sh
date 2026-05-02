#!/bin/sh
set -eu

CONFIG="${1:-/usr/local/etc/mediamtx.yml}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
RUNNER="${RUNNER:-${SCRIPT_DIR}/run-on-demand.sh}"

if [ "$(id -u)" -ne 0 ]; then
  echo "run as root: sudo $0" >&2
  exit 1
fi

if [ ! -f "$CONFIG" ]; then
  echo "MediaMTX config not found: $CONFIG" >&2
  exit 1
fi

if [ ! -x "$RUNNER" ]; then
  echo "runner is not executable: $RUNNER" >&2
  exit 1
fi

BACKUP="${CONFIG}.bak.$(date +%Y%m%d-%H%M%S)"
cp "$CONFIG" "$BACKUP"
echo "backup: $BACKUP"

tmp="$(mktemp)"
awk -v runner="$RUNNER" '
  /^  capture:/ { capture_seen = 1 }
  /^  all_others:/ && !inserted && !capture_seen {
    print "  capture:"
    print "    source: publisher"
    print "    runOnDemand: " runner
    print "    runOnDemandRestart: true"
    print "    runOnDemandStartTimeout: 20s"
    print "    runOnDemandCloseAfter: 5s"
    print ""
    inserted = 1
  }
  { print }
  END {
    if (capture_seen) {
      print "capture path already exists; leaving path section unchanged" > "/dev/stderr"
    } else if (!inserted) {
      print "could not find all_others path anchor" > "/dev/stderr"
      exit 2
    }
  }
' "$CONFIG" > "$tmp"

sed -i 's/^rtspTransports:.*/rtspTransports: [tcp]/' "$tmp"
install -m 644 "$tmp" "$CONFIG"
rm -f "$tmp"

systemctl disable --now rk-hdmi-streamer.service 2>/dev/null || true
systemctl restart mediamtx.service

echo "on-demand capture enabled for path: capture"
