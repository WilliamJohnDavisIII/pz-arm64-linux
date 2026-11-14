#!/bin/bash
# pz-arm64-linux installer: runs Project Zomboid (Build 42) natively on ARM64
# Linux, e.g. Apple Silicon Macs on Asahi Linux. Safe to rerun; it rebuilds
# everything from the game files and the inputs below.
set -euo pipefail
REPO=$(cd "$(dirname "$0")" && pwd)
PZA=${PZ_ARM64_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/pz-arm64}
BIN=${PZ_BIN_DIR:-$HOME/.local/bin}
APPS=${XDG_DATA_HOME:-$HOME/.local/share}/applications

FMOD_TGZ= GAME= STEAM_ROOT= NO_STEAM= DESKTOP=1 PWRULE= GLTEST=1
usage() {
  cat <<EOF
Usage: ./install.sh [options]
  --fmod FILE        FMOD Engine for Linux tarball (fmodstudioapi203xxlinux.tar.gz).
                     Default: newest one in ~/Downloads.
  --game DIR         The game's projectzomboid folder (default: found via Steam).
  --steam-root DIR   Steam install (default: ~/.local/share/Steam).
  --no-steam         Offline only: never start Steam (no multiplayer/Workshop).
  --no-desktop       Don't add an app-launcher entry.
  --pipewire-rule    Install an experimental PipeWire rule for the game's audio
                     stream (normal latency, no clipping on the stereo downmix).
  --skip-gl-test     Don't test OpenGL inside the VM (keeps the system Mesa).
EOF
}
while [ $# -gt 0 ]; do
  case $1 in
    --fmod) FMOD_TGZ=$2; shift ;;
    --game) GAME=$2; shift ;;
    --steam-root) STEAM_ROOT=$2; shift ;;
    --no-steam) NO_STEAM=1 ;;
    --no-desktop) DESKTOP= ;;
    --pipewire-rule) PWRULE=1 ;;
    --skip-gl-test) GLTEST= ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
  shift
done

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\n\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

step "Checking the system"
[ "$(uname -m)" = aarch64 ] || die "this is for ARM64 (aarch64) Linux; this machine is $(uname -m)."
PAGE=$(getconf PAGESIZE)
USE_MUVM=0; [ "$PAGE" != 4096 ] && USE_MUVM=1
echo "  page size $PAGE: $([ $USE_MUVM = 1 ] && echo 'single-player runs directly (pzldr), online play inside muvm (4K-page VM)' || echo 'running directly, no VM needed (untested)')"

MISSING=()
for c in gcc g++ patchelf python3 curl unzip rsync objdump pkg-config; do command -v $c >/dev/null || MISSING+=("$c"); done
[ $USE_MUVM = 1 ] && ! command -v muvm >/dev/null && MISSING+=(muvm)
for p in libavformat libavcodec libswscale libavutil x11 gl; do pkg-config --exists $p 2>/dev/null || MISSING+=("$p (dev files)"); done

JAVA_HOME=${PZ_JAVA_HOME:-}
if [ -z "$JAVA_HOME" ]; then
  for j in /usr/lib/jvm/java-25-openjdk /usr/lib/jvm/java-25-openjdk-* /usr/lib/jvm/java-25 /usr/lib/jvm/jdk-25*; do
    [ -x "$j/bin/javap" ] && [ -f "$j/include/jni.h" ] && { JAVA_HOME=$j; break; }
  done
fi
[ -n "$JAVA_HOME" ] || MISSING+=("Java 25 JDK")

if [ ${#MISSING[@]} -gt 0 ]; then
  echo "  missing: ${MISSING[*]}"
  echo
  if command -v pacman >/dev/null; then
    echo "  Install with:  sudo pacman -S --needed base-devel patchelf python curl unzip rsync ffmpeg libx11 mesa jdk25-openjdk muvm"
  elif command -v dnf >/dev/null; then
    echo "  Install with:  sudo dnf install gcc gcc-c++ patchelf python3 curl unzip rsync binutils pkgconf-pkg-config \\"
    echo "                   libavformat-free-devel libavcodec-free-devel libswscale-free-devel libavutil-free-devel \\"
    echo "                   libX11-devel mesa-libGL-devel java-25-openjdk-devel muvm"
  fi
  die "install the missing packages above (or set PZ_JAVA_HOME), then rerun."
fi
echo "  build tools OK, Java: $JAVA_HOME"

step "Finding Steam and the game"
if [ -z "$STEAM_ROOT" ]; then
  for s in "$HOME/.local/share/Steam" "$HOME/.steam/steam"; do [ -d "$s/steamapps" ] && { STEAM_ROOT=$(readlink -f "$s"); break; }; done
fi
if [ -z "$GAME" ]; then
  LIBS=()
  [ -n "$STEAM_ROOT" ] && LIBS+=("$STEAM_ROOT")
  if [ -n "$STEAM_ROOT" ] && [ -f "$STEAM_ROOT/steamapps/libraryfolders.vdf" ]; then
    while IFS= read -r l; do LIBS+=("$l"); done < <(grep -oP '"path"\s+"\K[^"]+' "$STEAM_ROOT/steamapps/libraryfolders.vdf")
  fi
  for l in "${LIBS[@]}"; do
    [ -f "$l/steamapps/common/ProjectZomboid/projectzomboid/projectzomboid.jar" ] && { GAME=$l/steamapps/common/ProjectZomboid/projectzomboid; break; }
  done
fi
[ -n "$GAME" ] && [ -f "$GAME/projectzomboid.jar" ] ||
  die "Project Zomboid not found. Install it with Steam (Linux version), or pass --game /path/to/ProjectZomboid/projectzomboid."
GAME=$(readlink -f "$GAME")
[ -d "$GAME/natives/android/arm64-v8a" ] || die "$GAME has no natives/android/arm64-v8a: this needs Build 42 or later."
echo "  game: $GAME"

STEAM_CMD=
if [ -z "$NO_STEAM" ]; then
  if [ -z "$STEAM_ROOT" ] || [ ! -f "$STEAM_ROOT/linuxarm64/steamclient.so" ]; then
    echo "  note: no native ARM64 Steam client (linuxarm64/steamclient.so) found; the game will run offline."
  elif command -v steam-muvm >/dev/null; then STEAM_CMD=steam-muvm
  elif [ $USE_MUVM = 1 ]; then STEAM_CMD="muvm -e LIBGL_ALWAYS_SOFTWARE=1 -- steam"
  else STEAM_CMD=steam
  fi
  [ -n "$STEAM_CMD" ] && echo "  Steam: $STEAM_CMD"
fi

step "Setting up FMOD (audio)"
if [ -z "$FMOD_TGZ" ]; then
  FMOD_TGZ=$(ls -t "$HOME"/Downloads/fmodstudioapi*linux.tar.gz 2>/dev/null | head -1 || true)
fi
if [ -z "$FMOD_TGZ" ] || [ ! -f "$FMOD_TGZ" ]; then
  cat <<EOF
  The game's audio engine (FMOD) has no ARM64 build in the game files, and its
  licence doesn't allow redistributing it, so download it yourself (free):
    1. Create an account at https://www.fmod.com and sign in.
    2. Go to Download -> FMOD Engine -> version 2.03.x -> Linux.
    3. Save the .tar.gz to ~/Downloads (or pass --fmod FILE) and rerun this.
EOF
  die "FMOD Engine for Linux not found."
fi
[[ $(basename "$FMOD_TGZ") =~ ^fmodstudioapi([0-9])([0-9]{2})([0-9]{2})linux\.tar\.gz$ ]] ||
  die "$FMOD_TGZ doesn't look like an FMOD Engine Linux download (fmodstudioapiXXXXXlinux.tar.gz)."
FMOD_VERSION=$(printf '0x%x%s%s' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${BASH_REMATCH[3]}")
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
tar xzf "$FMOD_TGZ" -C "$T"
CORE=$(find "$T" -path '*/api/core/lib/arm64' -type d | head -1)
STUDIO=$(find "$T" -path '*/api/studio/lib/arm64' -type d | head -1)
[ -n "$CORE" ] && [ -n "$STUDIO" ] || die "no arm64 libraries inside $FMOD_TGZ."
rm -rf "$PZA/fmod"; mkdir -p "$PZA/fmod/lib" "$PZA/fmod/debug"
cp -P "$CORE"/libfmod.so* "$STUDIO"/libfmodstudio.so* "$PZA/fmod/lib/"
cp -P "$CORE"/libfmodL.so* "$STUDIO"/libfmodstudioL.so* "$PZA/fmod/debug/"
# Logging builds under the regular names, for PZ_FMOD_DEBUG=1.
(cd "$PZA/fmod/debug" && for l in libfmodL.so.* libfmodstudioL.so.*; do
   [ -L "$l" ] && continue; n=${l/L.so/.so}; ln -sf "$l" "${n%.*}"; done)
echo "$FMOD_VERSION" > "$PZA/fmod/VERSION"
echo "  FMOD $(basename "$FMOD_TGZ" | grep -oP '\d{5}') arm64 libraries installed"

step "Installing files to $PZA"
mkdir -p "$PZA" "$BIN"
rm -rf "$PZA/src" "$PZA/tools"
cp -r "$REPO/src" "$REPO/tools" "$PZA/"
install -m755 "$REPO/launcher/run-pz.sh" "$PZA/run-pz.sh"
install -m755 "$REPO/launcher/vm-cachetrim.sh" "$PZA/vm-cachetrim.sh"
install -m755 "$REPO/launcher/zomboid" "$BIN/zomboid"
MESA_DIR=$(. "$PZA/config.env" 2>/dev/null; echo "${MESA_DIR:-}")
write_config() {
  cat > "$PZA/config.env" <<EOF
# Written by pz-arm64-linux install.sh. Rerun it to regenerate.
GAME="$GAME"
STEAM_ROOT="$STEAM_ROOT"
JAVA_HOME="$JAVA_HOME"
USE_MUVM=$USE_MUVM
STEAM_CMD="$STEAM_CMD"
MESA_DIR="$MESA_DIR"
EOF
}
write_config

step "Fetching ARM64 LWJGL (graphics/input library)"
"$PZA/tools/fetch-lwjgl.sh"

step "Building the native layer"
"$PZA/tools/build-natives.sh"

if [ -n "$GLTEST" ]; then
  step "Testing OpenGL$([ $USE_MUVM = 1 ] && echo ' inside the VM')"
  gcc -O2 -o "$PZA/tools/glxtest" "$REPO/src/shim/glxtest.c" -lGL -lX11
  gltest() {  # $1: optional private Mesa root
    local env=(env -u LIBGL_ALWAYS_SOFTWARE)
    [ -n "${1:-}" ] && env+=("LD_LIBRARY_PATH=$1/usr/lib" "LIBGL_DRIVERS_PATH=$1/usr/lib/dri")
    if [ $USE_MUVM = 1 ]; then
      # muvm doesn't hand back output when it joins an already-running VM, so
      # the test writes to a file and marks completion.
      local e=() r=$PZA/tools/glxtest.out
      [ -n "${1:-}" ] && e=(-e "LD_LIBRARY_PATH=$1/usr/lib" -e "LIBGL_DRIVERS_PATH=$1/usr/lib/dri")
      rm -f "$r"
      timeout 60 muvm "${e[@]}" -- sh -c "unset LIBGL_ALWAYS_SOFTWARE; '$PZA/tools/glxtest' > '$r.tmp' 2>&1; echo done >> '$r.tmp'; mv '$r.tmp' '$r'" >/dev/null 2>&1 || true
      for _ in $(seq 1 30); do [ -f "$r" ] && break; sleep 1; done
      cat "$r" 2>/dev/null; rm -f "$r"
    else
      timeout 30 "${env[@]}" "$PZA/tools/glxtest" 2>&1
    fi
  }
  # Pass = a hardware context that renders (llvmpipe is the CPU fallback: too slow).
  glok() { grep -q 'finish ok' <<<"$1" && ! grep -qE 'renderer=(llvmpipe|softpipe)' <<<"$1"; }
  OUT=$(gltest "$MESA_DIR" || true)
  if glok "$OUT"; then
    echo "  $(grep -m1 renderer= <<<"$OUT")"
  elif [ -z "$MESA_DIR" ] && command -v pacman >/dev/null; then
    echo "  The system Mesa failed or hung. Fetching the repo's stable Mesa privately for the game"
    echo "  (nothing is installed system-wide)."
    URL=$(pacman -Sp --print-format '%l' extra/mesa 2>/dev/null | head -1)
    [ -n "$URL" ] || die "couldn't look up the mesa package URL."
    mkdir -p "$PZA/mesa"; P=$PZA/mesa/$(basename "$URL")
    curl -fsSL -o "$P" "$URL" && curl -fsSL -o "$P.sig" "$URL.sig" || die "Mesa download failed."
    pacman-key --verify "$P.sig" "$P" >/dev/null 2>&1 || die "Mesa package signature check failed."
    rm -rf "$PZA/mesa/root"; mkdir -p "$PZA/mesa/root"; tar xf "$P" -C "$PZA/mesa/root" 2>/dev/null; rm -f "$P" "$P.sig"
    OUT=$(gltest "$PZA/mesa/root" || true)
    glok "$OUT" || die "OpenGL still fails with the stable Mesa:"$'\n'"$OUT"
    MESA_DIR=$PZA/mesa/root; write_config
    echo "  private Mesa works: $(grep -m1 renderer= <<<"$OUT")"
  else
    echo "  warning: OpenGL test failed; the game may not start:"; sed 's/^/    /' <<<"$OUT"
  fi
fi

if [ -n "$PWRULE" ]; then
  step "Installing the experimental PipeWire rule for the game's audio"
  mkdir -p "$HOME/.config/pipewire/pipewire-pulse.conf.d"
  cp "$REPO/config/50-pz-fmod.conf" "$HOME/.config/pipewire/pipewire-pulse.conf.d/"
  echo "  Takes effect after you log out and back in (or: systemctl --user restart pipewire-pulse)."
fi

if [ -n "$DESKTOP" ]; then
  mkdir -p "$APPS"
  entry() {  # $1 file, $2 name, $3 comment, $4 exec
    printf '%s\n' "[Desktop Entry]" "Name=$2" "Comment=$3" "Exec=$4" "Icon=$GAME/projectzomboid.png" \
      "Terminal=false" "Type=Application" "Categories=Game;" > "$APPS/$1"
  }
  if [ -n "$STEAM_CMD" ]; then
    entry pz-arm64.desktop "Project Zomboid" "Single-player, native ARM64 (pz-arm64-linux)" "env PZ_NOSTEAM=1 $BIN/zomboid"
    entry pz-arm64-online.desktop "Project Zomboid (Online)" "Steam, multiplayer and Workshop (pz-arm64-linux)" "$BIN/zomboid"
  else
    rm -f "$APPS/pz-arm64-online.desktop"
    entry pz-arm64.desktop "Project Zomboid" "Native ARM64 (pz-arm64-linux)" "$BIN/zomboid"
  fi
fi

step "Done"
echo "  Launch \"Project Zomboid\" from your app launcher, or run: zomboid"
echo "  Log: ${XDG_STATE_HOME:-$HOME/.local/state}/pz-arm64/run.log"
case ":$PATH:" in *":$BIN:"*) ;; *) echo "  note: $BIN is not on your PATH." ;; esac
