#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
temp_dir="$(mktemp -d)"
trap 'rm -rf "$temp_dir"' 0
mkdir "$temp_dir/bin"

real_install="$(command -v install)"
real_mv="$(command -v mv)"
export real_install real_mv

cat >"$temp_dir/bin/install" <<'EOF'
#!/bin/sh
[ "$fail_write" = 0 ] || exit 1
exec "$real_install" "$@"
EOF
cat >"$temp_dir/bin/mv" <<'EOF'
#!/bin/sh
[ "$fail_write" = 0 ] || exit 1
exec "$real_mv" "$@"
EOF
cat >"$temp_dir/bin/systemctl" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"$systemctl_log"
[ "$1" != restart ] || exit "$restart_status"
EOF
cat >"$temp_dir/bin/sudo" <<'EOF'
#!/bin/sh
exec "$@"
EOF
cat >"$temp_dir/bin/v4l2-ctl" <<'EOF'
#!/bin/sh
exit 1
EOF
chmod +x "$temp_dir/bin/"*

cat >"$temp_dir/original.conf" <<'EOF'
[main]
device=/dev/video-test
audio-device=hw:CARD=Video,DEV=0
stream-profile=mjpeg
width=1920
audio=false
video-range=full
rtsp-idle-grace-ms=8000

[profile.extra]
# Retain this profile comment.
path=/extra
codec=h265
video-range=limited
EOF

run_configure() {
    cp "$temp_dir/original.conf" "$temp_dir/current.conf"
    : >"$temp_dir/systemctl.log"
    # Keep both devices, select the custom profile, and change only its width.
    # Keep the audio and RTSP settings, then approve writing the file.
    printf '\n\n5\n\n1536\n\n\n\n\n\n\n\n\n\n\n\ny\n%s\n' "$1" |
        PATH="$temp_dir/bin:$PATH" CONFIG_FILE="$temp_dir/current.conf" \
        SERVICE_NAME=rk-configure-test.service systemctl_log="$temp_dir/systemctl.log" \
        fail_write="$2" restart_status="$3" \
        sh "$root/scripts/configure.sh" >"$temp_dir/output" 2>&1
}

preserves_unmanaged_options_and_profile() {
    run_configure n 0 0 || return 1
    config="$temp_dir/current.conf"
    awk '
        /^\[/ { section = $0 }
        section == "[main]" && $0 == "audio=false" { audio = 1 }
        section == "[main]" && $0 == "video-range=full" { range = 1 }
        section == "[main]" && $0 == "rtsp-idle-grace-ms=8000" { grace = 1 }
        section == "[main]" && /^width=/ { ++width_count; width = $0 }
        section == "[profile.extra]" && $0 == "path=/extra" { path = 1 }
        section == "[profile.extra]" && $0 == "codec=h265" { codec = 1 }
        section == "[profile.extra]" && $0 == "video-range=limited" { profile_range = 1 }
        END {
            exit !(audio && range && grace && width_count == 1 && width == "width=1536" &&
                   path && codec && profile_range)
        }
    ' "$config" || return 1
    [ ! -s "$temp_dir/systemctl.log" ]
}

keeps_original_and_skips_restart_when_write_fails() {
    if run_configure y 1 0; then return 1; fi
    cmp -s "$temp_dir/original.conf" "$temp_dir/current.conf" || return 1
    [ ! -s "$temp_dir/systemctl.log" ] || return 1
    ! grep -q 'Wrote ' "$temp_dir/output"
}

reports_a_failed_restart() {
    if run_configure y 0 1; then return 1; fi
    grep -qx 'restart rk-configure-test.service' "$temp_dir/systemctl.log" || return 1
    ! grep -q 'Restarted ' "$temp_dir/output"
}

failures=0
for test_case in preserves_unmanaged_options_and_profile \
    keeps_original_and_skips_restart_when_write_fails reports_a_failed_restart; do
    if "$test_case"; then
        printf 'PASS %s\n' "$test_case"
    else
        printf 'FAIL %s\n' "$test_case" >&2
        cat "$temp_dir/output" >&2
        failures=$((failures + 1))
    fi
done
[ "$failures" -eq 0 ]
