#!/usr/bin/env python3
"""Rebuilds Dungeon Defenders: Second Wave 7.6 for Android (com.trendy-7.6.apk, 2012, 32-bit ARM only)
into a 64-bit APK for modern Android (7 .. 17), including 64-bit-only chips (Snapdragon 8 Elite, ...).

Works on Windows, macOS and Linux. Needs only Java (17+) and Python (3.8+); apktool, the APK signer
and our prebuilt 64-bit loader are downloaded and checksum-verified on first run.

    python build.py path/to/com.trendy-7.6.apk                 arm64 APK for phones and tablets
    python build.py APK --abis arm64-v8a,x86_64                also x86_64, for the Android emulator
    python build.py APK --from-source                          build the loader (port/) yourself:
                                                               needs git, cmake, ninja, Android NDK r28
"""
import argparse
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WORK = ROOT / "build"
TOOLS = ROOT / "tools"
OUT = ROOT / "DunDefSW-64.apk"
ORIG_SHA256 = "821d019fd43bcc71befcc2a91da31809f375b8ec811155f3540abab2c27ea1b6"
NVIDIA = "https://http.download.nvidia.com/tegrazone/payload/dungeondefenders/DunDef3"

# third-party tools, pinned
APKTOOL = ("https://github.com/iBotPeaches/Apktool/releases/download/v2.12.1/apktool_2.12.1.jar",
           "66cf4524a4a45a7f56567d08b2c9b6ec237bcdd78cee69fd4a59c8a0243aeafa")
SIGNER = ("https://github.com/patrickfav/uber-apk-signer/releases/download/v1.3.0/uber-apk-signer-1.3.0.jar",
          "e1299fd6fcf4da527dd53735b56127e8ea922a321128123b9c32d619bba1d835")
# our loader (port/), built from this repository and attached to a GitHub release
LOADER_URL = os.environ.get(
    "DDPORT_LOADER_URL", "https://github.com/Santi2065/dundef-sw-android64/releases/download/loader-v1")
LOADER_SHA256 = {
    "arm64-v8a": "4b4909264168fe34defb8c970ad60d0c1a55b79e7e256edd64e1106b761e3627",
    "x86_64": "ec88218e16468f701576153032cd044cdf25638e08f9c1ea7eb7428f6e4b2fd8",
}

# signing key: a fixed local keystore, so rebuilt APKs install over each other without losing saves
KEYSTORE = ROOT / "dundef.keystore"
KS_ALIAS, KS_PASS = "dundef", "dundef123"


def die(msg):
    sys.exit("error: " + msg)


def run(*cmd, **kw):
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def download(url, dest, digest=None):
    if dest.exists() and (digest is None or sha256(dest) == digest):
        return dest
    print("downloading", url)
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".part")
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "ddport"})) as r, \
                open(tmp, "wb") as f:
            shutil.copyfileobj(r, f)
    except OSError as e:
        # python.org builds on macOS often lack CA certificates; curl ships with macOS and Windows 10+
        if not shutil.which("curl"):
            die(f"download failed: {e}")
        run("curl", "-fL", "-o", tmp, url)
    if digest and sha256(tmp) != digest:
        tmp.unlink()
        die(f"checksum mismatch for {url}")
    tmp.replace(dest)
    return dest


def java_tool(name):
    """keytool/java next to the java on PATH (a JRE-only PATH entry may lack keytool)."""
    exe = shutil.which(name)
    if exe:
        return exe
    java = shutil.which("java")
    if java:
        for cand in Path(java).resolve().parent.glob(name + "*"):
            return str(cand)
    die(f"'{name}' not found: install a JDK 17+ (e.g. https://adoptium.net) and make sure it is on PATH")


# ---------------------------------------------------------------- text patches
def read(path):
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def write(path, text):
    path.write_text(text, encoding="utf-8", newline="\n")


def patch(path, pattern, repl, flags=0, count=1):
    """Regex patch that must match. repl may be a string or a function of the match."""
    text, n = re.subn(pattern, repl, read(path), count=count, flags=flags)
    if n == 0:
        die(f"patch did not apply to {path.name}: {pattern[:70]}...  (is this the 7.6 APK?)")
    write(path, text)


def insert_after(path, header_pattern, code, flags=0):
    """Inserts smali lines right after the first match of header_pattern."""
    patch(path, header_pattern, lambda m: m.group(0) + code, flags=flags)


def patch_apk_sources(src):
    smali = src / "smali" / "com" / "trendy" / "ddapp"
    manifest = src / "AndroidManifest.xml"

    # targetSdk 28 is the lowest Android 14+ runs without the "built for an older version" warning;
    # nothing in the Java side changes behaviour up to there. The loader is built for API 24.
    patch(src / "apktool.yml", r"targetSdkVersion: .*", "targetSdkVersion: 28")
    patch(src / "apktool.yml", r"minSdkVersion: .*", "minSdkVersion: 24")
    patch(manifest, r"<application ",
          '<application android:extractNativeLibs="true" android:resizeableActivity="false" ')
    patch(manifest, r'android:configChanges="[^"]*"',
          'android:configChanges="locale|keyboard|keyboardHidden|navigation|orientation|screenLayout'
          '|screenSize|smallestScreenSize|uiMode|density"', count=0)
    patch(manifest, r"<activity ", '<activity android:resizeableActivity="false" ')
    patch(manifest, r"</application>",
          '<uses-library android:name="org.apache.http.legacy" android:required="false"/>'
          '<meta-data android:name="android.max_aspect" android:value="2.4"/></application>')

    # game data: Trendy's servers are dead, NVIDIA's mirror only answers on HTTPS
    write(src / "assets" / "DownloadInfo.xml",
          f'<Info Type="Download" PushURL="{NVIDIA}" Version="701" BinaryVersion="1">\n'
          f'  <URL Path="{NVIDIA}/" Priority="1" />\n</Info>\n')

    # Market billing v2 is gone; bindService with an implicit intent throws on API 21+
    patch(smali / "BillingService.smali",
          re.escape(".catch Ljava/lang/SecurityException; {:try_start_0 .. :try_end_0} :catch_0"),
          ".catch Ljava/lang/Exception; {:try_start_0 .. :try_end_0} :catch_0", count=0)

    # NVIDIA's CDN answers chunked (no Content-Length) and HttpDownload treats size -1 as an error.
    # BufferedHttpEntity reads such responses into memory (pieces are <= 5 MB) and reports the length.
    insert_after(smali / "HttpDownload.smali",
                 r"getEntity\(\)Lorg/apache/http/HttpEntity;\n\n    move-result-object v2\n",
                 "\n    new-instance v3, Lorg/apache/http/entity/BufferedHttpEntity;\n"
                 "    invoke-direct {v3, v2}, Lorg/apache/http/entity/BufferedHttpEntity;-><init>(Lorg/apache/http/HttpEntity;)V\n"
                 "    invoke-interface {v1, v3}, Lorg/apache/http/HttpResponse;->setEntity(Lorg/apache/http/HttpEntity;)V\n"
                 "    move-object v2, v3\n")

    ddapp = smali / "ddapp.smali"
    # EGL config pick: the game scores configs by distance to RGB565 but starts from a threshold only
    # an exact 565 config can beat. Modern GPUs (Adreno 7xx/8xx) offer no 565 configs, so nothing
    # matched ("OpenGL initialization failed"). Start from INT_MAX so the closest config always wins.
    patch(ddapp, r"(\.method public JavaCallback_initEGL\(.*?)const v6, 0xf4240",
          lambda m: m.group(1) + "const v6, 0x7fffffff", flags=re.S)

    # render at full resolution (the engine asks for ~50% to spare 2012 GPUs), hide the nav bar and
    # ask for the panel's 144 Hz mode (ignored where unsupported; the loader lifts the 60 fps cap)
    insert_after(ddapp, r"\.method public JavaCallback_SetFixedSizeScale\(F\)V\n    \.locals 2\n\n    \.prologue\n",
                 "    const/high16 p1, 0x3f800000\n")
    insert_after(ddapp, r"\.method public onWindowFocusChanged\(Z\)V\n    \.locals 4\n\n    \.prologue\n",
                 "    invoke-virtual {p0}, Lcom/trendy/ddapp/ddapp;->getWindow()Landroid/view/Window;\n"
                 "    move-result-object v0\n"
                 "    invoke-virtual {v0}, Landroid/view/Window;->getAttributes()Landroid/view/WindowManager$LayoutParams;\n"
                 "    move-result-object v1\n"
                 "    const/high16 v2, 0x43100000\n"
                 "    iput v2, v1, Landroid/view/WindowManager$LayoutParams;->preferredRefreshRate:F\n"
                 "    invoke-virtual {v0, v1}, Landroid/view/Window;->setAttributes(Landroid/view/WindowManager$LayoutParams;)V\n"
                 "    invoke-virtual {v0}, Landroid/view/Window;->getDecorView()Landroid/view/View;\n"
                 "    move-result-object v0\n"
                 "    const/16 v1, 0x1706\n"
                 "    invoke-virtual {v0, v1}, Landroid/view/View;->setSystemUiVisibility(I)V\n")

    on_create = (r"\.method public onCreate\(Landroid/os/Bundle;\)V\n    \.locals 3\n(?:.*\n)*?"
                 r"    invoke-super \{p0, p1\}, Landroid/app/Activity;->onCreate\(Landroid/os/Bundle;\)V\n")
    # LAN games are found by UDP broadcast on port 14001; many phones drop broadcast/multicast
    # Wi-Fi packets unless the app holds a MulticastLock
    patch(manifest, r'<uses-permission android:name="android.permission.INTERNET"/>',
          lambda m: m.group(0) + '<uses-permission android:name="android.permission.CHANGE_WIFI_MULTICAST_STATE"/>')
    insert_after(ddapp, on_create,
                 "    invoke-virtual {p0}, Lcom/trendy/ddapp/ddapp;->getApplicationContext()Landroid/content/Context;\n"
                 "    move-result-object v0\n"
                 '    const-string v1, "wifi"\n'
                 "    invoke-virtual {v0, v1}, Landroid/content/Context;->getSystemService(Ljava/lang/String;)Ljava/lang/Object;\n"
                 "    move-result-object v0\n"
                 "    check-cast v0, Landroid/net/wifi/WifiManager;\n"
                 "    if-eqz v0, :ddport_no_wifi\n"
                 '    const-string v1, "dundef-lan"\n'
                 "    invoke-virtual {v0, v1}, Landroid/net/wifi/WifiManager;->createMulticastLock(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;\n"
                 "    move-result-object v0\n"
                 "    invoke-virtual {v0}, Landroid/net/wifi/WifiManager$MulticastLock;->acquire()V\n"
                 "    :ddport_no_wifi\n")
    # tablet HUD fix (patches/smali/AspectFix.smali)
    insert_after(ddapp, on_create, "    invoke-static {p0}, Lcom/trendy/ddapp/AspectFix;->apply(Landroid/app/Activity;)V\n")

    # whole files: text input fix, tablet aspect fix, adaptive launcher icon
    for f in (ROOT / "patches").rglob("*"):
        if f.is_file():
            dest = src / f.relative_to(ROOT / "patches")
            dest.parent.mkdir(parents=True, exist_ok=True)
            write(dest, read(f))


# ---------------------------------------------------------------- native loader
def ndk_dir():
    if os.environ.get("NDK"):
        return Path(os.environ["NDK"])
    homes = [os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT"),
             Path.home() / "Android" / "Sdk", Path.home() / "Library" / "Android" / "sdk",
             Path(os.environ.get("LOCALAPPDATA", "")) / "Android" / "Sdk"]
    for h in filter(None, homes):
        found = sorted(glob.glob(str(Path(h) / "ndk" / "28.*")))
        if found:
            return Path(found[-1])
    die("Android NDK r28 not found (set NDK=/path/to/ndk or ANDROID_HOME)")


def build_loader(abi):
    """Builds port/ for one ABI and returns the stripped library."""
    for tool in ("git", "cmake", "ninja"):
        if not shutil.which(tool):
            die(f"--from-source needs '{tool}' on PATH")
    ndk = ndk_dir()
    run("git", "-C", ROOT, "submodule", "update", "--init", "port/third_party/dynarmic")
    run("git", "-C", ROOT / "port/third_party/dynarmic", "submodule", "update", "--init", "--depth", "1",
        *[f"externals/{e}" for e in ("fmt", "mcl", "oaknut", "robin-map", "xbyak", "zycore", "zydis")])
    boost = ROOT / "port/third_party/boost_1_86_0"
    if not (boost / "boost").is_dir():
        tar = download("https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.bz2",
                       TOOLS / "boost_1_86_0.tar.bz2")
        with tarfile.open(tar) as t:
            members = [m for m in t.getmembers() if m.name.startswith("boost_1_86_0/boost/")]
            if hasattr(tarfile, "data_filter"):
                t.extractall(boost.parent, members, filter="data")
            else:
                t.extractall(boost.parent, members)
    build = ROOT / "port" / f"build-{abi}"
    if not (build / "build.ninja").exists():
        run("cmake", "-S", ROOT / "port", "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_TOOLCHAIN_FILE={ndk / 'build/cmake/android.toolchain.cmake'}",
            f"-DANDROID_ABI={abi}", "-DANDROID_PLATFORM=android-24", "-DANDROID_STL=c++_static",
            stdout=subprocess.DEVNULL)
    run("ninja", "--quiet", "-C", build, "DunDef-Android")
    strip = glob.glob(str(ndk / "toolchains/llvm/prebuilt/*/bin/llvm-strip*"))[0]
    out = build / "libDunDef-Android.stripped.so"
    run(strip, "-o", out, build / "libDunDef-Android.so")
    return out


def prebuilt_loader(abi):
    if abi not in LOADER_SHA256:
        die(f"no prebuilt loader for {abi} (use --from-source)")
    return download(f"{LOADER_URL}/libDunDef-Android-{abi}.so",
                    TOOLS / "loader-v1" / f"libDunDef-Android-{abi}.so", LOADER_SHA256[abi])


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("apk", type=Path, help="your own copy of com.trendy-7.6.apk")
    ap.add_argument("--abis", default="arm64-v8a", help="comma-separated: arm64-v8a, x86_64")
    ap.add_argument("--from-source", action="store_true", help="build the loader instead of downloading it")
    args = ap.parse_args()

    if not args.apk.is_file():
        die(f"{args.apk} not found (pass your own copy of the game's APK)")
    if sha256(args.apk) != ORIG_SHA256:
        print("warning: this is not the exact 7.6 APK these patches were written for", file=sys.stderr)
    java = java_tool("java")
    apktool = download(APKTOOL[0], TOOLS / "apktool_2.12.1.jar", APKTOOL[1])
    signer = download(SIGNER[0], TOOLS / "uber-apk-signer-1.3.0.jar", SIGNER[1])
    abis = [a.strip() for a in args.abis.split(",") if a.strip()]
    loaders = {abi: build_loader(abi) if args.from_source else prebuilt_loader(abi) for abi in abis}

    shutil.rmtree(WORK, ignore_errors=True)
    src = WORK / "src"
    run(java, "-jar", apktool, "d", "-q", "-f", "-o", src, args.apk.resolve())
    patch_apk_sources(src)

    # native code: our 64-bit loader takes the game's library name; the untouched 32-bit
    # library is shipped next to it as data for the loader
    guest = src / "lib" / "armeabi" / "libDunDef-Android.so"
    for abi, lib in loaders.items():
        (src / "lib" / abi).mkdir(parents=True)
        shutil.copyfile(lib, src / "lib" / abi / "libDunDef-Android.so")
        shutil.copyfile(guest, src / "lib" / abi / "libDunDefGuest.so")
    shutil.rmtree(src / "lib" / "armeabi")
    (src / "unknown" / "com" / "trendy" / "ddapp" / "ddapp.java.bak").unlink()

    unsigned = WORK / "unsigned.apk"
    run(java, "-jar", apktool, "b", "-q", "-o", unsigned, src)

    if not KEYSTORE.exists():
        run(java_tool("keytool"), "-genkeypair", "-keystore", KEYSTORE, "-storepass", KS_PASS,
            "-keypass", KS_PASS, "-alias", KS_ALIAS, "-keyalg", "RSA", "-keysize", "2048",
            "-validity", "36500", "-dname", "CN=DunDef SW Port", stdout=subprocess.DEVNULL)
    signed_dir = WORK / "signed"
    run(java, "-jar", signer, "-a", unsigned, "-o", signed_dir, "--ks", KEYSTORE, "--ksAlias", KS_ALIAS,
        "--ksPass", KS_PASS, "--ksKeyPass", KS_PASS, stdout=subprocess.DEVNULL)
    shutil.copyfile(next(signed_dir.glob("*.apk")), OUT)
    print(f"OK -> {OUT}")
    print("Keep dundef.keystore: updates must be signed with it to install over the app without losing saves.")


if __name__ == "__main__":
    main()
