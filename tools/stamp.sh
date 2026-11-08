#!/bin/bash
# Fingerprint of the game files the native layer depends on: the Android libs
# and LWJGL's version class. When this changes, the launcher rebuilds.
PZA=${PZ_ARM64_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/pz-arm64}
# shellcheck source=/dev/null
. "$PZA/config.env"
{
  stat -c '%n %s %Y' "$GAME"/natives/android/arm64-v8a/*.so
  unzip -p "$GAME/projectzomboid.jar" org/lwjgl/Version.class | sha256sum
  sha256sum "$PZA"/src/shim/*.c "$PZA"/src/steam/*
} 2>/dev/null | sha256sum | cut -d' ' -f1
