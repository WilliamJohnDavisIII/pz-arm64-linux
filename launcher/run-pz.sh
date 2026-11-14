#!/bin/bash
# Starts Project Zomboid on native ARM64 Java. Use the `zomboid` launcher.
# On 16K-page kernels the game's Android libs can't be loaded by glibc (they need 4K pages):
# single-player runs directly on the host with pzldr loading them (PZ_NATIVE=1, set by the
# launcher), and Steam/online play runs inside muvm, a 4K-page VM.
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
LIBS=$PZA/natives; JLIBS=$PZA/natives
# No-VM mode: pzldr's stubs come first, so Java loads them instead of the Android libs.
[ -n "$PZ_NATIVE" ] && { LIBS=$PZA/natives16k:$LIBS; JLIBS=$PZA/natives16k:$JLIBS; }
# A private Mesa, when the system one hangs inside the VM (see install.sh). Not used on the host.
if [ -n "$MESA_DIR" ] && [ -z "$PZ_NATIVE" ]; then
  LIBS=$LIBS:$MESA_DIR/usr/lib
  export LIBGL_DRIVERS_PATH=$MESA_DIR/usr/lib/dri
fi
export LD_LIBRARY_PATH=$LIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
# PZ_FMOD_DEBUG=1 uses FMOD's logging builds (warnings go to the log).
[ -n "$PZ_FMOD_DEBUG" ] && export LD_LIBRARY_PATH=$PZA/fmod/debug:$LD_LIBRARY_PATH

# Steam mode needs the Steam client in this VM. PZ_NOSTEAM=1 forces offline.
STEAM=1; NOSTEAM=
if [ -n "$PZ_NOSTEAM" ] || [ -n "$PZ_NATIVE" ] || [ -z "$STEAM_CMD" ]; then STEAM=0; NOSTEAM=-nosteam
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

# G1 rather than the game's default ZGC: G1 stores object references in 4 bytes (ZGC
# can't), which cuts the heap a lot on big maps, and compact object headers save more.
# Idle memory goes back to the system every 30 s. PZ_GC_OPTS replaces these.
GC_OPTS=${PZ_GC_OPTS:--XX:+UseG1GC -XX:+UseCompactObjectHeaders -XX:G1PeriodicGCInterval=30000}

export LD_PRELOAD=$PZA/natives/libpztls.so${LD_PRELOAD:+:$LD_PRELOAD}
# shellcheck disable=SC2086
exec "$JAVA_HOME/bin/java" \
  -Djava.awt.headless=true --enable-native-access=ALL-UNNAMED \
  --add-exports=java.base/jdk.internal.misc=ALL-UNNAMED \
  -Xmx${PZ_MEMORY:-4096m} -Dzomboid.steam=$STEAM -Dzomboid.znetlog=1 \
  -Djava.library.path="$JLIBS" -Djava.security.egd=file:/dev/./urandom \
  $GC_OPTS -XX:-OmitStackTraceInFastThrow $ZB $PZ_JAVA_OPTS \
  -cp "$PZA/lwjgl/lwjgl-arm64.jar:.:projectzomboid.jar" \
  zombie.gameStates.MainScreenState $NOSTEAM "$@" > "$LOG" 2>&1
