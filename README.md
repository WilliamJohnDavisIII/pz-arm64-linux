# pz-arm64-linux

Run **Project Zomboid (Build 42)** natively on **ARM64 Linux**, such as Apple Silicon Macs running Asahi Linux. You get GPU rendering, sound, menu videos, Steam login, the server browser and online play.

Project Zomboid has no official ARM64 Linux build, and on Asahi the x86 version doesn't work under emulation (FEX): Java's JIT compiler crashes. However, the game ships **Android ARM64 builds of its own native libraries**. This project makes those libraries run on regular Linux and starts the game on a native ARM64 Java.

> Tested on one machine: a MacBook Air M1 running Asahi Linux (Arch Linux ARM based, Omarchy), game build 42.21. Other distros and ARM64 machines are untested. Reports are welcome.

## Install

1. **Install the game through Steam** (its Linux version) and let it finish downloading.
2. **Download FMOD**: the game's audio engine has no ARM64 build in the game files, and its licence doesn't allow redistributing it. It's free:
   - Create an account at [fmod.com](https://www.fmod.com) and sign in.
   - Go to **Download → FMOD Engine → 2.03.x → Linux**, and save the `.tar.gz` to `~/Downloads`.
3. **Run the installer:**
   ```sh
   git clone https://github.com/WilliamJohnDavisIII/pz-arm64-linux
   cd pz-arm64-linux
   ./install.sh
   ```
   If build tools are missing, it tells you exactly what to install, then you rerun it.
4. **Launch "Project Zomboid"** from your app launcher, or run `zomboid`.

The installer is safe to rerun. Run `./install.sh --help` for options, such as `--game DIR`, `--fmod FILE` or `--no-steam`.

### Requirements
- ARM64 (aarch64) Linux.
- A kernel with **16K pages** (Asahi) also needs **[muvm](https://github.com/AsahiLinux/muvm)**, a lightweight VM with 4K pages, because the game's Android libraries require 4K pages. On 4K-page kernels the game runs without muvm, but that's untested.
- gcc/g++, patchelf, python3, curl, unzip, binutils, pkg-config, the ffmpeg and X11/GL development files, and a **Java 25 JDK**.
- For online play: Steam's native ARM64 client (`~/.local/share/Steam/linuxarm64/steamclient.so`). Without it, the game runs offline.

## What works

| | |
|---|---|
| GPU-accelerated rendering (OpenGL 4.6 on the M1) | ✅ |
| Music, sound effects, voice-chat mic | ✅ |
| Animated menu backgrounds | ✅ |
| Steam login, server browser, joining servers (crossplay with x86 players) | ✅ |
| Workshop mods you're subscribed to in Steam | ✅ |
| ZombieBuddy (Java mod loader), if subscribed | ✅ |
| Hosting a server from the Host menu | ⚠️ untested |
| Browsing/subscribing to Workshop items in-game | ⚠️ untested |

## How it works

- **Android libraries on Linux.** The installer copies the game's `natives/android/arm64-v8a` libraries and patches them with `patchelf`. A small compatibility library, `libpzshim.so`, covers the places where Android's C library (bionic) differs from glibc: stdio, `sysconf`, network and terminal structures, and thread-structure sizes and alignment. `libpztls.so` reserves the thread-local slot that bionic reads its stack canary from.
- **LWJGL.** Official linux-arm64 LWJGL libraries are downloaded from Maven Central for the game's exact LWJGL version, and checked against the checksums the game's own jar records.
- **Graphics.** On some systems, Mesa hangs creating an OpenGL context inside muvm. The installer tests this. If it fails on Arch, it downloads the repo's stable Mesa package, verifies its signature, and uses it privately for the game only. Nothing is installed system-wide.
- **Menu videos.** The game's videos are Bink 1, which has no ARM64 library. `binkplayer.c` reimplements the game's video interface on ffmpeg.
- **Audio.** These are FMOD's ARM64 libraries from your download. The game insists on the exact FMOD version it was built with, which the installer reads from the game's files, so the shim reports that version. FMOD releases within a minor series (2.03.x) are binary compatible.
- **Steam.** The game's Steam bridge (`libZNetJNI64.so`) is x86-only, so `src/steam` is a from-scratch ARM64 reimplementation. It talks to Valve's native ARM64 `steamclient.so` and is wire-compatible with normal x86 clients and servers.

## Game updates

Steam keeps updating the game as usual, and the launcher plays it straight from Steam's folder. Before each launch, the launcher checks whether an update changed the game's native libraries or its LWJGL version. If so, it re-patches and rebuilds automatically, which takes about a minute. If the game ever moves to a new FMOD minor version (2.04, say), the launcher stops and asks you to download the matching FMOD. A larger change to the game's networking code could need updates here.

## Switches

Set these when running `zomboid`:

| Variable | Effect |
|---|---|
| `PZ_NOSTEAM=1` | Start offline, without Steam |
| `PZ_NOZB=1` | Don't load ZombieBuddy |
| `PZ_UI_SCALE=2` | Scale ZombieBuddy's approval dialog on HiDPI screens |
| `PZ_MEMORY=4096m` | Java heap size (default `3072m`) |
| `PZ_FMOD_DEBUG=1` | Use FMOD's logging libraries |
| `PZ_FMOD_NATIVE_FORMAT=1` | Keep the game's own 5.1 / 32 kHz audio format instead of 48 kHz stereo |
| `PZ_JAVA_OPTS="..."` | Extra JVM options |

The log is `~/.local/state/pz-arm64/run.log`, with `run.log.prev` for the run before.

## Known issues

- **Apple Silicon speakers can go silent at game launch.** On the M1, `speakersafetyd` (the speaker-protection daemon) sometimes crashed with `Negative power, bad ivsense data?` when the game opened its audio. When that happens, the kernel locks the speakers to protect them. Before the current mitigation, this happened on roughly half of launches. Headphones keep working.
  - **Current mitigation (unconfirmed):** the game asks FMOD for an unusual 5.1, 32 kHz output, so the shim makes FMOD output plain 48 kHz stereo instead. Since then there have been no crashes, but over only a few launches so far. `PZ_FMOD_NATIVE_FORMAT=1` restores the game's own format.
  - **If it still happens,** restore the speakers with:
    ```sh
    sudo systemctl reset-failed speakersafetyd && sudo systemctl start speakersafetyd
    ```
    If that doesn't help, reboot. **Never bypass speaker protection.**
  - `--pipewire-rule` installs an experimental PipeWire rule for the game's audio stream: it sets a normal latency and stops the downmix clipping. On its own, it did **not** prevent the crash.
- **Restarting PipeWire** can make some apps (Electron apps, for example) lose their microphone until they're restarted.
- **Mods** that patch Java (through ZombieBuddy) must match your game build, just as on any other platform.

## Uninstall

```sh
./uninstall.sh
```

This removes everything the installer added. The game, Steam and your saves (`~/Zomboid`) are left alone.

## Legal

This project contains only original code. It doesn't include or redistribute any game files, FMOD, Steam or Mesa binaries: the installer uses your own copies or downloads them from their official sources. Project Zomboid is a trademark of The Indie Stone; this project isn't affiliated with or endorsed by The Indie Stone or Valve.

MIT licensed. See [LICENSE](LICENSE).
