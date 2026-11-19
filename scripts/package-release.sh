#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
VERSION="${1:-$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || date +%Y%m%d)}"
case "$VERSION" in
  *[!abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-]*)
    printf 'Invalid release version: %s\n' "$VERSION" >&2
    exit 1
    ;;
esac
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
DIST_DIR="${DIST_DIR:-$ROOT/dist}"
NAME="rockchip-hdmi-capture-rtsp-${VERSION}-linux-aarch64"

set -- cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DRK_BUILD_STREAMER=ON -DBUILD_TESTING=OFF
if [ -n "${RGA_ROOT:-}" ]; then
  set -- "$@" "-DRGA_ROOT=$RGA_ROOT"
fi
if [ -n "${RK_CPU:-}" ]; then
  set -- "$@" "-DRK_CPU=$RK_CPU"
fi
"$@"
cmake --build "$BUILD_DIR" -j"$JOBS"

binary="$BUILD_DIR/rk-hdmi-streamer"
machine="$(LC_ALL=C readelf -h "$binary" | awk '/Machine:/ { sub(/^[^:]*:[[:space:]]*/, ""); print }')"
if [ "$machine" != "AArch64" ]; then
  printf 'Expected an AArch64 executable, got %s\n' "${machine:-unknown architecture}" >&2
  exit 1
fi

mkdir -p "$DIST_DIR"
tmp="$(mktemp -d "$DIST_DIR/.release.XXXXXX")"
trap 'rm -rf -- "$tmp"' 0
trap 'exit 1' HUP INT TERM
stage="$tmp/$NAME"
mkdir -p "$stage/bin" "$stage/man" "$stage/completions" "$stage/systemd" "$stage/scripts"

install -m 755 "$binary" "$stage/bin/rk-hdmi-streamer"
install -m 644 "$ROOT/man/rk-hdmi-streamer.1" "$stage/man/rk-hdmi-streamer.1"
install -m 644 "$ROOT/completions/rk-hdmi-streamer.bash" "$stage/completions/rk-hdmi-streamer.bash"
install -m 644 "$ROOT/LICENSE" "$ROOT/README.md" "$ROOT/CHANGELOG.md" "$stage"
install -m 644 "$ROOT/systemd/rk-hdmi-streamer.conf" "$ROOT/systemd/rk-hdmi-streamer-direct.service" "$stage/systemd"
install -m 755 "$ROOT/scripts/configure.sh" "$stage/scripts"

if [ -n "${RGA_ROOT:-}" ] && [ -f "$RGA_ROOT/libs/Linux/gcc-aarch64/librga.so" ]; then
  mkdir -p "$stage/third_party/librga/aarch64"
  install -m 755 "$RGA_ROOT/libs/Linux/gcc-aarch64/librga.so" "$stage/third_party/librga/aarch64/librga.so"
  if [ -f "$RGA_ROOT/COPYING" ]; then
    install -m 644 "$RGA_ROOT/COPYING" "$stage/third_party/librga/COPYING"
  fi
fi

archive="$NAME.tar.gz"
tar -C "$tmp" -czf "$tmp/$archive" "$NAME"
(cd "$tmp" && sha256sum "$archive" > "$archive.sha256")
mv -fT -- "$tmp/$archive" "$DIST_DIR/$archive"
mv -fT -- "$tmp/$archive.sha256" "$DIST_DIR/$archive.sha256"

printf '%s\n' "$DIST_DIR/$archive" "$DIST_DIR/$archive.sha256"
