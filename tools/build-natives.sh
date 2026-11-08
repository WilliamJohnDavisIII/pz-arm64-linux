#!/bin/bash
# Build the ARM64 native layer for Project Zomboid into $PZA/natives:
# patch the game's Android arm64 libs for glibc, add the FMOD arm64 libs, and
# compile the shims, video player and Steam bridge.
# Run by install.sh, and by the launcher whenever a game update changes the
# game's native libs.
set -euo pipefail
PZA=${PZ_ARM64_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/pz-arm64}
# shellcheck source=/dev/null
. "$PZA/config.env"

SRC=$PZA/src
AND=$GAME/natives/android/arm64-v8a
OUT=$PZA/natives
B=$PZA/build
JI="-I$JAVA_HOME/include -I$JAVA_HOME/include/linux"
say() { printf '  %s\n' "$*"; }

[ -d "$AND" ] || { echo "error: $AND not found (is the game installed and up to date?)" >&2; exit 1; }
rm -rf "$OUT.new" "$B"; mkdir -p "$OUT.new" "$B"

# The FMOD release PZ's integration was built against: FMOD::Studio::System::create
# is called with the header version as a constant (mov w1,#lo; movk w1,#hi,lsl #16).
CALL=$(objdump -d --no-show-raw-insn "$AND/libfmodintegration64.so" | grep -m1 -B4 'Studio6System6createEPPS1_j@plt>$' || true)
LO=$(grep -oP 'mov\s+w1, #\K0x[0-9a-f]+' <<<"$CALL" | tail -1)
HI=$(grep -oP 'movk\s+w1, #\K0x[0-9a-f]+(?=, lsl #16)' <<<"$CALL" | tail -1)
[ -n "$LO" ] && [ -n "$HI" ] || { echo "error: could not detect the game's FMOD version" >&2; exit 1; }
FMOD_VER=$(printf '0x%x' $((HI * 65536 + LO)))
say "game expects FMOD $(printf '%d.%02x.%02x' $((FMOD_VER >> 16)) $(((FMOD_VER >> 8) & 0xff)) $((FMOD_VER & 0xff)))"
HAVE=$(cat "$PZA/fmod/VERSION")
if [ $((FMOD_VER >> 8)) -ne $((HAVE >> 8)) ]; then
  printf 'error: the game now needs FMOD %d.%02x.x, but the installed FMOD SDK is %d.%02x.x.\n' \
    $((FMOD_VER >> 16)) $(((FMOD_VER >> 8) & 0xff)) $((HAVE >> 16)) $(((HAVE >> 8) & 0xff)) >&2
  echo "Download the matching FMOD Engine for Linux from fmod.com and rerun install.sh --fmod <file>." >&2
  exit 2
fi

say "compiling shims"
gcc -O2 -fPIC -shared -Wall -DPZ_FMOD_VERSION="$FMOD_VER" -o "$OUT.new/libpzshim.so" "$SRC/shim/pzshim.c" -ldl
gcc -O2 -fPIC -shared -Wall -o "$OUT.new/libpztls.so" "$SRC/shim/pztls.c"
gcc -O2 -fPIC -shared -o "$OUT.new/libPZXInitThreads64.so" "$SRC/shim/xinit.c" -lX11
gcc -O2 -fPIC -shared -Wall $JI -o "$OUT.new/libbink64.so" "$SRC/shim/binkplayer.c" \
  -lavformat -lavcodec -lswscale -lavutil -lpthread
echo 'void pz_placeholder(void){}' > "$B/empty.c"
gcc -O2 -fPIC -shared -o "$OUT.new/libBink2x64.so" "$B/empty.c"   # Bink 2 is unused (videos are Bink 1)
gcc -O2 -fPIC -shared -o "$OUT.new/libsteam_api.so" "$B/empty.c"  # the bridge talks to steamclient.so directly

say "patching the game's Android libs"
# Bionic symbols whose glibc counterparts differ in layout or semantics go to pzsh_* wrappers.
: > "$B/renames.txt"
for s in sysconf pthread_attr_init pthread_mutex_init pthread_cond_init pthread_cond_destroy \
         pthread_cond_wait pthread_cond_timedwait pthread_cond_signal pthread_cond_broadcast \
         fflush fclose feof fgets fputc fputs fread fwrite fseek fseeko ftell ftello getc ungetc \
         rewind vfprintf fprintf tcgetattr tcsetattr getaddrinfo freeaddrinfo getnameinfo __fread_chk; do
  echo "$s pzsh_$s" >> "$B/renames.txt"
done
cp "$B/renames.txt" "$B/renames-fmod.txt"
echo "FMOD_System_GetVersion pzsh_FMOD_System_GetVersion" >> "$B/renames-fmod.txt"
for f in "$AND"/*.so; do
  b=$(basename "$f"); R=$B/renames.txt
  [ "$b" = libfmodintegration64.so ] && R=$B/renames-fmod.txt
  cp "$f" "$OUT.new/$b"
  patchelf --replace-needed libc.so libc.so.6 --replace-needed libm.so libm.so.6 \
    --replace-needed libdl.so libdl.so.2 --rename-dynamic-symbols "$R" \
    --add-needed libpzshim.so --set-rpath '$ORIGIN' "$OUT.new/$b"
  python3 -I "$SRC/shim/unversion.py" "$OUT.new/$b" >/dev/null
done

say "adding FMOD arm64 libs"
cp -P "$PZA"/fmod/lib/* "$OUT.new/"

say "building the Steam bridge"
g++ -std=c++17 -O2 -fPIC -shared -Wall -Wno-non-virtual-dtor -Wno-unused-function \
  -Wno-delete-non-virtual-dtor -Wno-stringop-truncation $JI \
  -o "$OUT.new/libZNetJNI64.so" "$SRC"/steam/*.cpp \
  -L"$OUT.new" -l:libZNetNoSteam64.so -l:libRakNet64.so -ldl -Wl,-rpath,'$ORIGIN' -Wl,--no-undefined

rm -rf "$OUT.old"; [ -d "$OUT" ] && mv "$OUT" "$OUT.old"
mv "$OUT.new" "$OUT"; rm -rf "$OUT.old" "$B"
"$PZA/tools/stamp.sh" > "$PZA/natives.stamp"
say "natives ready"
