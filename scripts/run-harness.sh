#!/bin/sh
# Runs the self-test inside the CrossOver bottle (memory guards, hooks, D3D11 + ImGui).
#   scripts/run-harness.sh [--no-render]
# CROSSOVER_APP / CX_BOTTLE override the CrossOver install and bottle name.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${CROSSOVER_APP:-}"
if [ -z "$APP" ]; then
    for candidate in "$HOME/Applications/CrossOver"*.app /Applications/CrossOver*.app; do
        if [ -x "$candidate/Contents/SharedSupport/CrossOver/bin/wine" ]; then
            APP="$candidate"
            break
        fi
    done
fi
WINE="$APP/Contents/SharedSupport/CrossOver/bin/wine"
[ -x "$WINE" ] || { echo "CrossOver not found (set CROSSOVER_APP)" >&2; exit 1; }
EXE="Z:$(printf '%s' "$ROOT/build/dps-harness.exe" | tr '/' '\\')"
rm -f "$ROOT/build/nativePC/plugins/MhwDpsMeter/imgui.ini" "$ROOT/build/nativePC/plugins/MhwDpsMeter/settings.json"
"$WINE" --bottle "${CX_BOTTLE:-Steam}" "$EXE" "$@" 2>&1 | grep -v ':fixme:'
