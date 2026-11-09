#!/bin/bash
# Starts Project Zomboid on native ARM64 Java. Runs inside muvm on 16K-page
# kernels (the game's Android libs need 4K pages). Use the `zomboid` launcher.
PZA=${PZ_ARM64_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/pz-arm64}
# shellcheck source=/dev/null
. "$PZA/config.env" || exit 1
STATE=${XDG_STATE_HOME:-$HOME/.local/state}/pz-arm64
LOG=${PZ_LOG:-$STATE/run.log}
mkdir -p "$STATE"; [ -f "$LOG" ] && mv -f "$LOG" "$LOG.prev"
cd "$GAME" || exit 1

# One instance at a time (two would clobber each other's saves and log).
if pgrep -f "zombie.gameStates.MainScreenState" >/dev/null; then echo "Project Zomboid is already running" >&2; exit 0; fi

unset LIBGL_ALWAYS_SOFTWARE
LIBS=$PZA/natives
# A private Mesa, when the system one hangs inside the VM (see install.sh).
if [ -n "$MESA_DIR" ]; then
  LIBS=$LIBS:$MESA_DIR/usr/lib
  export LIBGL_DRIVERS_PATH=$MESA_DIR/usr/lib/dri
fi
export LD_LIBRARY_PATH=$LIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
# PZ_FMOD_DEBUG=1 uses FMOD's logging builds (warnings go to the log).
[ -n "$PZ_FMOD_DEBUG" ] && export LD_LIBRARY_PATH=$PZA/fmod/debug:$LD_LIBRARY_PATH

# Steam mode needs the Steam client in this VM. PZ_NOSTEAM=1 forces offline.
STEAM=1; NOSTEAM=
if [ -n "$PZ_NOSTEAM" ] || [ -z "$STEAM_CMD" ]; then STEAM=0; NOSTEAM=-nosteam
else
  for _ in $(seq 1 45); do pgrep -x steamwebhelper >/dev/null && break; sleep 2; done
  pgrep -x steamwebhelper >/dev/null || { STEAM=0; NOSTEAM=-nosteam; echo "Steam not running: starting offline" >&2; }
fi
export SteamAppId=108600 SteamGameId=108600
export PZ_STEAMCLIENT=${PZ_STEAMCLIENT:-$STEAM_ROOT/linuxarm64/steamclient.so}

# ZombieBuddy (Java mod loader, Workshop 3619862853), if subscribed. Steam
# launch options don't apply here, so load it directly. PZ_NOZB=1 disables it.
ZB=; ZBSRC=${GAME%/common/*}/workshop/content/108600/3619862853/mods/ZombieBuddy/libs/ZombieBuddy.jar
if [ -z "$PZ_NOZB" ] && [ -f "$ZBSRC" ]; then
  cmp -s "$ZBSRC" ZombieBuddy.jar || cp -f "$ZBSRC" ZombieBuddy.jar
  ZB=-javaagent:ZombieBuddy.jar
  # Its approval dialog is a separate Swing window; PZ_UI_SCALE=2 suits HiDPI screens.
  [ -n "$PZ_UI_SCALE" ] && export JDK_JAVA_OPTIONS="-Dsun.java2d.uiScale=$PZ_UI_SCALE${JDK_JAVA_OPTIONS:+ $JDK_JAVA_OPTIONS}"
fi

export LD_PRELOAD=$PZA/natives/libpztls.so${LD_PRELOAD:+:$LD_PRELOAD}
# shellcheck disable=SC2086
exec "$JAVA_HOME/bin/java" \
  -Djava.awt.headless=true --enable-native-access=ALL-UNNAMED \
  --add-exports=java.base/jdk.internal.misc=ALL-UNNAMED \
  -Xmx${PZ_MEMORY:-4096m} -Dzomboid.steam=$STEAM -Dzomboid.znetlog=1 \
  -Djava.library.path="$PZA/natives" -Djava.security.egd=file:/dev/./urandom \
  -XX:+UseZGC -XX:-OmitStackTraceInFastThrow $ZB $PZ_JAVA_OPTS \
  -cp "$PZA/lwjgl/lwjgl-arm64.jar:.:projectzomboid.jar" \
  zombie.gameStates.MainScreenState $NOSTEAM "$@" > "$LOG" 2>&1
