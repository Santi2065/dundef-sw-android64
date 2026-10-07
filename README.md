# Dungeon Defenders: Second Wave on 64-bit Android

Runs the original 2012 Android release of *Dungeon Defenders: Second Wave* (`com.trendy.ddapp`,
version 7.6) on modern Android, including **64-bit-only chips that cannot execute 32-bit ARM
code at all**: Snapdragon 8 Elite, Dimensity 9300, Google Tensor G2 and newer, and so on.

**This repository contains no game code or assets.** You need your own copy of the original
APK. `build.sh` patches it on your machine and produces a new APK you can sideload.

## Status

| What | State |
|---|---|
| Menus, hero creation, saves, campaign levels | working |
| LAN co-op (host / browse) | working (hosting verified; two-device play not yet confirmed) |
| Internet multiplayer (GameSpy) | not possible: GameSpy shut down in 2014 |
| Uncapped frame rate | up to ~140 fps measured; 144 Hz requested on capable screens |

Tested on: Android 15 emulator, 64-bit only (x86_64 and arm64 builds), and a OnePlus Pad 3
(Snapdragon 8 Elite, Adreno 830), where it downloads its data and runs (menus, hero creation).
Mali and PowerVR GPUs are untested; reports are welcome.

## Why it no longer worked, and what this does about it

| Problem on modern Android | Fix |
|---|---|
| The game's engine is a 32-bit ARM library; new CPUs have no 32-bit mode | A 64-bit loader runs it through an ARM32→ARM64 JIT (see below) |
| `targetSdkVersion 11`: Android 14+ refuses to install it | targetSdk 28, minSdk 24 |
| Trendy's download servers are gone | Points the downloader at NVIDIA's mirror (HTTPS) |
| The mirror answers with chunked transfers and the 2012 downloader treats "unknown length" as an error | Responses are buffered (`BufferedHttpEntity`, pieces are ≤ 5 MB) |
| Old Market in-app billing throws on modern Android | Exception is caught |
| Modern GPUs (e.g. Adreno 7xx/8xx) expose no RGB565 EGL configs, so the game reported "OpenGL initialization failed" | The closest EGL config is always accepted |
| Rendered at ~50% resolution with a 60 fps engine cap | Full resolution, cap raised to 145 fps, 144 Hz display mode requested, immersive full screen |
| Phones drop Wi-Fi broadcast packets, so LAN games are hard to find | The app holds a `MulticastLock` |
| On touch keyboards, Enter typed a newline into hero/game names, and closing the keyboard discarded the text | Single-line field with a "Done" key; Done or closing the keyboard keeps the name |
| The 48 px legacy icon shows up small on a white plate | Adaptive icon built from the game's own 170 px artwork |
| On tablets (7" or more, the game's own test) the HUD only fits near 3:2: at 7:5 (e.g. OnePlus Pad 3) BUILD/HERO fall off the right edge, at 16:10 the bottom row is cut too | On tablets the window is sized to 1.48:1 and centered, with black bars top/bottom or left/right. |
| On phones wider than 16:9 (18:9 to 21:9) the phone HUD is left-aligned: BUILD/HERO and the action buttons sit mid-screen instead of at the right edge | The loader patches the engine's layout code to center the HUD; the scene stays full screen |

## How the loader works

`port/` builds a 64-bit `libDunDef-Android.so` that replaces the game's library. The untouched
32-bit original is shipped next to it as `libDunDefGuest.so`.

- **Memory**: a ~1.25 GB arena is reserved in a free gap of the process's low 4 GB. Guest
  pointers are host pointers, so most calls into libc and OpenGL pass straight through.
  `dlmalloc` serves the guest heap from that arena.
- **ELF loader**: maps the ARM32 library, applies its relocations and runs its static
  constructors. Every imported function becomes a two-instruction `svc #n; bx lr` stub.
- **CPU**: [dynarmic](https://github.com/azahar-emu/dynarmic) JIT-compiles ARM/Thumb code to
  arm64 (or x86_64 for the emulator), with one JIT per guest thread. Leaf imports run directly
  inside the JIT's SVC callback. Calls that can re-enter the guest (JNI, `qsort`) leave the JIT
  first and save and restore the context.
- **High-level emulation** of what the library imports (~280 functions): libc, math, stdio,
  sockets, pthreads (old bionic's 4-byte mutexes and condvars are mapped to real ones), zlib,
  OpenGL ES 2 and EGL. Where 32-bit and 64-bit layouts differ (`struct stat`, `tm`, `timeval`,
  `addrinfo`, `hostent`, varargs `printf`/`scanf`…) the arguments are translated.
- **JNI both ways**: the guest gets a fake `JavaVM`/`JNIEnv` living in guest memory, with Java
  objects and IDs as 32-bit handles. Java calls the guest's 18 native methods through host
  trampolines registered with ART.
- **Soft-float replaced**: the library was built without an FPU, so every float/double operation
  and every integer division (ARMv5 has no divide) was a call into libgcc routines running as
  dozens of emulated instructions; the game makes ~17 million of them per second. Each routine's
  first instruction is patched into a branch to a few ARM VFP/IDIV instructions that dynarmic
  compiles straight to host float/divide instructions (+48% uncapped fps in the tavern). A
  self-test checks them bit for bit against the original libgcc code.
- **Config tweak**: when the engine opens `Coalesced_*.bin` it gets a copy with
  `MaxSmoothedFrameRate` raised from 62 to 145.

The Java/smali patches are applied by `build.sh` with plain `sed`/`perl`. Each is commented
and checked.

## Step by step

Building runs on **Linux** (tested on Fedora). It should also work under WSL2 on Windows, but
that is untested. macOS is not supported (the script uses GNU `sed`).

### 1. Get your copy of the APK

The game was delisted from Google Play years ago, so you need the APK you already own. If it is
still installed on an old phone or tablet, enable USB debugging there and run:

```bash
adb shell pm path com.trendy.ddapp      # prints e.g. package:/data/app/com.trendy.ddapp-1.apk
adb pull /data/app/com.trendy.ddapp-1.apk com.trendy-7.6.apk   # use the path printed above
```

A backup made with a backup app or file manager works too. The patches were written for
version 7.6, SHA-256 `821d019fd43bcc71befcc2a91da31809f375b8ec811155f3540abab2c27ea1b6`.
`build.sh` warns if yours differs and carries on anyway.

### 2. Install the tools (once)

```bash
# Debian / Ubuntu
sudo apt install openjdk-17-jdk git curl perl cmake ninja-build unzip
# Fedora
sudo dnf install java-17-openjdk-devel git curl perl cmake ninja-build unzip
```

**Android SDK build-tools and NDK r28.** If you don't use Android Studio, download the
"Command line tools only" zip from <https://developer.android.com/studio#command-line-tools-only>, then:

```bash
mkdir -p ~/Android/Sdk/cmdline-tools
unzip commandlinetools-linux-*_latest.zip -d ~/Android/Sdk/cmdline-tools
mv ~/Android/Sdk/cmdline-tools/cmdline-tools ~/Android/Sdk/cmdline-tools/latest
yes | ~/Android/Sdk/cmdline-tools/latest/bin/sdkmanager --licenses
~/Android/Sdk/cmdline-tools/latest/bin/sdkmanager "build-tools;35.0.0" "ndk;28.2.13676358" "platform-tools"
```

**apktool** (<https://apktool.org>), installed into `~/.local/bin`, which must be in your `PATH`:

```bash
mkdir -p ~/.local/bin
curl -Lo ~/.local/bin/apktool.jar https://github.com/iBotPeaches/Apktool/releases/download/v2.12.1/apktool_2.12.1.jar
printf '#!/bin/sh\nexec java -jar "$(dirname "$0")/apktool.jar" "$@"\n' > ~/.local/bin/apktool
chmod +x ~/.local/bin/apktool
```

### 3. Build

Clone with `git`. GitHub's "Download ZIP" leaves out the dynarmic submodule and the build fails.

```bash
git clone https://github.com/Santi2065/dundef-sw-android64
cd dundef-sw-android64
./build.sh /path/to/com.trendy-7.6.apk
```

The first build downloads dynarmic and the Boost headers (about 150 MB) and takes a few minutes.
Later builds take seconds. The result is **`DunDefSW-64.apk`**.

The first build also creates `dundef.keystore`, the key the APK is signed with. **Keep it.**
Android only installs an update over the existing app if it is signed with the same key. With a
different key you would have to uninstall first, which deletes the game data and your saves.

Options: `ABIS="arm64-v8a x86_64"` also builds for x86_64 emulators. Set `ANDROID_HOME=`, `NDK=`
or `BUILD_TOOLS=` if your SDK is not under `~/Android/Sdk`.

### 4. Install on your device

1. If the original game is still installed on that device, uninstall it first (it is signed
   with a different key).
2. Copy `DunDefSW-64.apk` to the device and open it from a file manager, allowing "install
   unknown apps" when asked. Alternatively, with USB debugging on, run
   `adb install DunDefSW-64.apk`.
3. On first launch, accept the download. The game fetches about 860 MB over Wi-Fi into
   `Android/data/com.trendy.ddapp/files/DunDef`. Consider copying that folder to a computer
   afterwards: the game depends on NVIDIA keeping the files online.

To update later, rebuild and install the new APK over the old one. Your data and saves stay.
Uninstalling the app deletes both.

## Multiplayer

- **LAN**: Online → Adventure → Next → pick a hero → **LAN** → Custom → *Host Game* or
  *Refresh*. No account is needed. Every device must be on the same Wi-Fi network.
- **Internet**: the game used GameSpy, which shut down in 2014.
- **PVP Arena** was an in-app purchase. Google's original billing service no longer exists,
  so it cannot be bought.

## Known issues

- The tutorial level renders softer than other scenes; the cause is not known yet.
- In the Options menu the "Chase Camera" label is slightly clipped on the right (cosmetic).
- The Back button opens the game's own quit prompt (original behavior).

## Debugging

```bash
adb logcat -s ddport                        # loader messages + a "perf:" fps line every 5 s
adb shell setprop debug.ddport.novsync 1    # disable vsync to measure uncapped fps
adb shell setprop debug.ddport.hudalign 0   # wide-phone HUD: 0 = left (stock), 1 = centered (default), 2 = right
adb shell setprop debug.ddport.selftest 1   # compare the float/division replacements with libgcc at startup
adb shell setprop debug.ddport.profile 1    # log the most-called imports every 5 s
adb shell setprop debug.ddport.profile 2    # also count guest instructions per function (slow)
```

## Legal

This project is not affiliated with or endorsed by Trendy Entertainment, Chromatic Games or
NVIDIA. *Dungeon Defenders* is a trademark of its respective owner. This repository does not
contain or distribute any of the game's code or assets: you must own a copy of the game. The
game data is downloaded by the game itself from the mirror its original release used.

## Credits and licenses

- Code in this repository: [MIT](LICENSE)
- [dynarmic](https://github.com/azahar-emu/dynarmic): ARM JIT (0BSD), git submodule
- [dlmalloc](https://gee.cs.oswego.edu/dl/html/malloc.html) by Doug Lea (MIT-0), bundled in `port/third_party/dlmalloc`
- [Boost](https://www.boost.org) headers (BSL-1.0), downloaded at build time
- [apktool](https://apktool.org) (Apache-2.0), used to decode and rebuild the APK
