#!/bin/sh
set -eu

ui_dir="${ONYRION_UI_DIR:-/usr/share/onyrion/ui/ewwii}"
shell_bin="${ONYRION_SHELL_BIN:-onyrion-shell}"
export ONYRION_UI_DIR="$ui_dir"

# Onyrion may ship a private Ewwii/gtk4-layer-shell runtime when Shell-owned
# Group surfaces need lifecycle hooks that are not part of upstream Ewwii.
# Keep the system package untouched; prepend the private runtime only when the
# complete versioned directory is installed.
ewwii_runtime_dir="${ONYRION_EWWII_RUNTIME_DIR:-/usr/lib/onyrion/ewwii}"
if [ -x "$ewwii_runtime_dir/ewwii" ] &&
   [ -f "$ewwii_runtime_dir/lib/libgtk4-layer-shell.so.0" ]
then
    PATH="$ewwii_runtime_dir:$PATH"
    LD_LIBRARY_PATH="$ewwii_runtime_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export PATH LD_LIBRARY_PATH
    printf 'EWWII RUNTIME private=%s\n' "$ewwii_runtime_dir"
fi

# Cursor theme/size are session-owned. onyrion-session seeds both variables
# before starting Core and Shell, so Ewwii must only inherit that contract.
: "${XCURSOR_THEME:=default}"
: "${XCURSOR_SIZE:=24}"
export XCURSOR_THEME XCURSOR_SIZE

provider_pid=$$

# Bootstrap Listen lifecycle only; normal watch-state owns daily UI exactly once.
#
# Ewwii 0.10 does not start Listen handlers until a window exists. The previous
# one-shot `state --ewwii` solved that cycle by running daily-ui itself, but the
# first normal watch-state snapshot then ran the same ownership pass again.
# Ewwii replaced the just-opened bar while plugin Custom children were still
# parented to it, producing gtk_box_append(child already has parent).
#
# Wait for plugin registration, open one inert 1x1 seed window, then let normal
# watch-state perform the first and only background/bar ownership pass. Close
# the seed after the real bar becomes active.
(
    attempt=0
    plugin_ready_streak=0
    seed_open=0

    while [ "$attempt" -lt 240 ]; do
        if timeout 0.5s ewwii --config "$ui_dir" ping >/dev/null 2>&1 &&
           timeout 0.75s ewwii --config "$ui_dir" list-plugins 2>/dev/null |
               grep -Fq 'io.onyrion.shell.ewwii'
        then
            plugin_ready_streak=$((plugin_ready_streak + 1))

            if [ "$plugin_ready_streak" -ge 5 ] && [ "$seed_open" -eq 0 ]; then
                if ewwii --config "$ui_dir" open onyrion-bootstrap >/dev/null 2>&1
                then
                    seed_open=1
                    printf 'EWWII BOOTSTRAP seed=open attempt=%s plugin-ready-streak=%s\n'                         "$attempt" "$plugin_ready_streak"
                fi
            fi

            if [ "$seed_open" -eq 1 ] &&
               timeout 0.75s ewwii --config "$ui_dir" active-windows 2>/dev/null |
                   grep -Fq 'onyrion-bar-0:'
            then
                ewwii --config "$ui_dir" close onyrion-bootstrap >/dev/null 2>&1 || true
                printf 'EWWII BOOTSTRAP listen-seed=ready attempt=%s\n' "$attempt"
                exit 0
            fi
        else
            plugin_ready_streak=0
        fi

        attempt=$((attempt + 1))
        sleep 0.05
    done

    if [ "$seed_open" -eq 1 ]; then
        ewwii --config "$ui_dir" close onyrion-bootstrap >/dev/null 2>&1 || true
    fi

    echo "FAIL: Ewwii Listen seed bootstrap did not converge" >&2

    # The parent is exec-replaced by Ewwii, so this remains the supervised
    # provider PID. Force the existing provider restart policy to retry.
    kill -TERM "$provider_pid" 2>/dev/null || true
    exit 1
) &

exec ewwii \
    --config "$ui_dir" \
    --no-daemonize \
    daemon
