#!/usr/bin/env bash
# Run inside xvfb-run. Pinned JUCE8 needs WM_PROTOCOLS to exist before its
# first peer; bare Xvfb supplies no window manager to initialise that atom.
set -euo pipefail
uikit_wm_log=$(mktemp)
openbox --sm-disable >"$uikit_wm_log" 2>&1 &
uikit_wm_pid=$!
trap 'kill "$uikit_wm_pid" 2>/dev/null || true; rm -f "$uikit_wm_log"' EXIT
for ((uikit_attempt = 0; uikit_attempt < 50; ++uikit_attempt)); do
    if [[ $(xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null) == *"window id"* ]]; then
        "$@"
        exit 0
    fi
    sleep 0.1
done
cat "$uikit_wm_log" >&2
printf '%s\n' 'The UI test window manager did not become ready.' >&2
exit 1
