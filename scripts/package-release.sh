#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
VERSION="${1:-$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || date +%Y%m%d)}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
DIST_DIR="${DIST_DIR:-$ROOT/dist}"
NAME="rockchip-hdmi-capture-rtsp-${VERSION}-linux-aarch64"
STAGE="$DIST_DIR/$NAME"

cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$JOBS"

rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/systemd" "$STAGE/scripts"

install -m 755 "$BUILD_DIR/rk-hdmi-streamer" "$STAGE/bin/rk-hdmi-streamer"
install -m 644 "$ROOT/LICENSE" "$STAGE/LICENSE"
install -m 644 "$ROOT/README.md" "$STAGE/README.md"
install -m 644 "$ROOT/CHANGELOG.md" "$STAGE/CHANGELOG.md"
install -m 644 "$ROOT/systemd/rk-hdmi-streamer.env" "$STAGE/systemd/rk-hdmi-streamer.env"
install -m 644 "$ROOT/systemd/rk-hdmi-streamer-direct.service" "$STAGE/systemd/rk-hdmi-streamer-direct.service"
install -m 644 "$ROOT/systemd/rk-hdmi-streamer.service" "$STAGE/systemd/rk-hdmi-streamer.service"
install -m 644 "$ROOT/systemd/mediamtx.service" "$STAGE/systemd/mediamtx.service"
install -m 755 "$ROOT/scripts/run-on-demand.sh" "$STAGE/scripts/run-on-demand.sh"
install -m 755 "$ROOT/scripts/enable-on-demand.sh" "$STAGE/scripts/enable-on-demand.sh"

tar -C "$DIST_DIR" -czf "$DIST_DIR/$NAME.tar.gz" "$NAME"
(cd "$DIST_DIR" && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256")

echo "$DIST_DIR/$NAME.tar.gz"
echo "$DIST_DIR/$NAME.tar.gz.sha256"
