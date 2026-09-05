#!/bin/sh
set -eu

ui_dir="${ONYRION_UI_DIR:-/usr/share/onyrion/ui/ewwii}"
shell_bin="${ONYRION_SHELL_BIN:-onyrion-shell}"
export ONYRION_UI_DIR="$ui_dir"
provider_pid=$$

# Bootstrap state, not UI ownership.
#
# Ewwii 0.10 starts Listen handlers with window lifecycle, so a daemon with no
# windows cannot bootstrap the onyrion_state Listen by itself. Once IPC is
# ready, request one authoritative Shell snapshot and project it through the
# same C daily-ui owner used by continuous watch-state. That owner opens the
# per-output backgrounds/bars; opening the first window then activates the
# normal Nbcl Listen for subsequent updates.
(
    attempt=0

    while [ "$attempt" -lt 100 ]; do
        if ewwii --config "$ui_dir" ping >/dev/null 2>&1; then
            if "$shell_bin" state --ewwii "$ui_dir"
            then
                printf 'EWWII BOOTSTRAP state-sync=ready attempt=%s\n' "$attempt"
                exit 0
            fi
        fi

        attempt=$((attempt + 1))
        sleep 0.05
    done

    echo "FAIL: Ewwii authoritative state bootstrap did not converge" >&2

    # The parent is exec-replaced by Ewwii, so this remains the supervised
    # provider PID. Force the existing provider restart policy to retry the
    # complete daemon + one-shot state bootstrap unit.
    kill -TERM "$provider_pid" 2>/dev/null || true
    exit 1
) &

exec ewwii \
    --config "$ui_dir" \
    --no-daemonize \
    daemon
