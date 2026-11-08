#!/bin/bash
# Download the official linux-arm64 LWJGL natives matching the game's LWJGL
# version, check every library against the SHA-1 the game's own jar records for
# it, and pack them into $PZA/lwjgl/lwjgl-arm64.jar (put first on the classpath).
set -euo pipefail
PZA=${PZ_ARM64_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/pz-arm64}
# shellcheck source=/dev/null
. "$PZA/config.env"
JAR=$GAME/projectzomboid.jar

V=$("$JAVA_HOME/bin/javap" -constants -cp "$JAR" org.lwjgl.Version |
  awk -F'= |;' '/VERSION_MAJOR/{a=$2} /VERSION_MINOR/{b=$2} /VERSION_REVISION/{c=$2} END{print a"."b"."c}')
[[ $V =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "error: could not read the game's LWJGL version" >&2; exit 1; }
echo "  game uses LWJGL $V"

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/dl" "$T/root"
# Modules whose natives the game bundles for linux/x64.
MODS=$(unzip -Z1 "$JAR" 'linux/x64/org/lwjgl/*' | awk -F/ '$5 != "" {print ($5 ~ /\.so$/) ? "lwjgl" : "lwjgl-"$5}' | sort -u)
for m in $MODS; do
  f=$m-$V-natives-linux-arm64.jar
  curl -fsSL -o "$T/dl/$f" "https://repo1.maven.org/maven2/org/lwjgl/$m/$V/$f" ||
    { echo "error: download failed: $f" >&2; exit 1; }
  unzip -q -o "$T/dl/$f" 'linux/arm64/*' -d "$T/root"
done
# Verify against the checksums shipped inside projectzomboid.jar.
n=0
while IFS= read -r so; do
  want=$(unzip -p "$JAR" "META-INF/$so.sha1" 2>/dev/null | tr -d '[:space:]')
  got=$(sha1sum "$T/root/$so" | cut -d' ' -f1)
  if [ -z "$want" ]; then echo "  warning: no recorded checksum for $so"; continue; fi
  [ "$want" = "$got" ] || { echo "error: checksum mismatch for $so" >&2; exit 1; }
  n=$((n + 1))
done < <(cd "$T/root" && find linux -name '*.so')
echo "  verified $n libraries against the game's checksums"
mkdir -p "$PZA/lwjgl"
rm -f "$PZA/lwjgl/lwjgl-arm64.jar"; (cd "$T/root" && python3 -I -m zipfile -c "$PZA/lwjgl/lwjgl-arm64.jar" linux)
echo "$V" > "$PZA/lwjgl/VERSION"
