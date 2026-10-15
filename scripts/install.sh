#!/bin/sh
# Copies build/MhwDpsMeter.dll into the game's nativePC/plugins folder.
#   scripts/install.sh            install (or update)
#   scripts/install.sh uninstall  remove the DLL (settings and fight logs are kept)
# Set MHW_DIR to the game folder (or CX_BOTTLE to the bottle name) if it is not found.
set -eu

GAME_SUBDIR="drive_c/Program Files (x86)/Steam/steamapps/common/Monster Hunter World"
if [ -n "${MHW_DIR:-}" ]; then
    GAME_DIR="$MHW_DIR"
else
    # A bottle named "Steam" in a custom bottle folder or CrossOver's default one.
    GAME_DIR="$HOME/CXPBottles/Steam/$GAME_SUBDIR"
    for bottles in "$HOME/CXPBottles" "$HOME/Library/Application Support/CrossOver/Bottles"; do
        if [ -f "$bottles/${CX_BOTTLE:-Steam}/$GAME_SUBDIR/MonsterHunterWorld.exe" ]; then
            GAME_DIR="$bottles/${CX_BOTTLE:-Steam}/$GAME_SUBDIR"
            break
        fi
    done
fi
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DLL="$ROOT/build/MhwDpsMeter.dll"
TARGET="$GAME_DIR/nativePC/plugins/MhwDpsMeter.dll"

if [ ! -f "$GAME_DIR/MonsterHunterWorld.exe" ]; then
    echo "MonsterHunterWorld.exe not found in: $GAME_DIR" >&2
    echo "Set MHW_DIR to the folder that contains it." >&2
    exit 1
fi

if [ "${1:-}" = "uninstall" ]; then
    rm -f "$TARGET"
    echo "removed $TARGET"
    echo "(settings and fight logs stay in $GAME_DIR/nativePC/plugins/MhwDpsMeter)"
    exit 0
fi

if [ ! -f "$DLL" ]; then
    echo "build the plugin first: make" >&2
    exit 1
fi
if [ ! -f "$GAME_DIR/dinput8.dll" ] || [ ! -f "$GAME_DIR/loader.dll" ]; then
    echo "warning: Stracker's Loader (dinput8.dll + loader.dll) is not in the game folder;" >&2
    echo "         nothing will load the plugin." >&2
fi

mkdir -p "$GAME_DIR/nativePC/plugins"
cp "$DLL" "$TARGET"
echo "installed $TARGET"
