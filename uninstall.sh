#!/bin/bash
# Remove everything install.sh added. The game, your saves (~/Zomboid) and
# Steam are left alone.
set -eu
PZA=${PZ_ARM64_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/pz-arm64}
BIN=${PZ_BIN_DIR:-$HOME/.local/bin}
APPS=${XDG_DATA_HOME:-$HOME/.local/share}/applications
GAME=$( . "$PZA/config.env" 2>/dev/null; echo "${GAME:-}")
rm -rf "$PZA" "${XDG_STATE_HOME:-$HOME/.local/state}/pz-arm64"
rm -f "$BIN/zomboid" "$APPS/pz-arm64.desktop" "$HOME/.config/pipewire/pipewire-pulse.conf.d/50-pz-fmod.conf"
# ZombieBuddy.jar copied into the game folder by the launcher (Steam's "verify files" would also remove it).
[ -n "$GAME" ] && rm -f "$GAME/ZombieBuddy.jar"
echo "Removed pz-arm64-linux. Workshop mod links in ~/Zomboid/mods were left in place."
