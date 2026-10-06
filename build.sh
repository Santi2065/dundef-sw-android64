#!/usr/bin/env bash
# Rebuilds Dungeon Defenders: Second Wave 7.6 for Android (com.trendy-7.6.apk, 2012, 32-bit ARM only)
# into a 64-bit APK for modern Android (7 .. 17), including 64-bit-only chips (Snapdragon 8 Elite, ...).
# Our loader (port/) runs the original 32-bit game library through a JIT (dynarmic).
#   ./build.sh path/to/com.trendy-7.6.apk                       arm64 APK for real devices
#   ABIS="arm64-v8a x86_64" ./build.sh path/to/com.trendy-7.6.apk   also x86_64, for the emulator
# Needs (Linux): git, curl, java, perl, apktool, cmake, ninja, Android SDK build-tools + NDK r28.
set -euo pipefail

ORIG=${1:-$(dirname "$0")/com.trendy-7.6.apk}
[ -f "$ORIG" ] || { echo "usage: $0 path/to/com.trendy-7.6.apk   (your own copy of the game)" >&2; exit 1; }
ORIG=$(realpath "$ORIG")
cd "$(dirname "$0")"

ORIG_SHA256=821d019fd43bcc71befcc2a91da31809f375b8ec811155f3540abab2c27ea1b6
OUT=DunDefSW-64.apk
ABIS=${ABIS:-arm64-v8a}
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
BT=${BUILD_TOOLS:-$(ls -d "$SDK"/build-tools/* 2>/dev/null | sort -V | tail -1)}
NDK=${NDK:-$(ls -d "$SDK"/ndk/28.* 2>/dev/null | sort -V | tail -1)}
NVIDIA=https://http.download.nvidia.com/tegrazone/payload/dungeondefenders/DunDef3
W=build

[ "$(sha256sum "$ORIG" | cut -d' ' -f1)" = "$ORIG_SHA256" ] ||
  echo "warning: $ORIG is not the exact 7.6 APK these patches were written for" >&2
[ -x "$BT/apksigner" ] || { echo "Android build-tools not found under $SDK (set ANDROID_HOME or BUILD_TOOLS)" >&2; exit 1; }
[ -d "$NDK" ] || { echo "Android NDK r28 not found under $SDK/ndk (set NDK=/path/to/ndk)" >&2; exit 1; }
command -v apktool >/dev/null || { echo "apktool not found in PATH" >&2; exit 1; }

# --- dependencies (not vendored): dynarmic is a git submodule, Boost headers are downloaded once
git submodule update --init port/third_party/dynarmic
git -C port/third_party/dynarmic submodule update --init --depth 1 \
  externals/fmt externals/mcl externals/oaknut externals/robin-map externals/xbyak externals/zycore externals/zydis
[ -d port/third_party/boost_1_86_0/boost ] ||
  curl -fsSL https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.bz2 |
  tar xj -C port/third_party boost_1_86_0/boost

rm -rf "$W" && apktool d -q -f -o "$W/src" "$ORIG"
S=$W/src
SMALI=$S/smali/com/trendy/ddapp

# --- manifest: targetSdk 28 is the lowest that Android 14+ runs without the "built for an older
# version" warning; nothing in the Java side changes behaviour up to there.
sed -i -e 's/targetSdkVersion: .*/targetSdkVersion: 28/' -e 's/minSdkVersion: .*/minSdkVersion: 24/' "$S/apktool.yml"  # loader is built for API 24
sed -i \
  -e 's#<application #<application android:extractNativeLibs="true" android:resizeableActivity="false" #' \
  -e 's#android:configChanges="[^"]*"#android:configChanges="locale|keyboard|keyboardHidden|navigation|orientation|screenLayout|screenSize|smallestScreenSize|uiMode|density"#' \
  -e 's#<activity #<activity android:resizeableActivity="false" #' \
  -e 's#</application>#<uses-library android:name="org.apache.http.legacy" android:required="false"/><meta-data android:name="android.max_aspect" android:value="2.4"/></application>#' \
  "$S/AndroidManifest.xml"

# --- game data: Trendy's servers are dead, NVIDIA's mirror only answers on HTTPS
cat > "$S/assets/DownloadInfo.xml" <<EOF
<Info Type="Download" PushURL="$NVIDIA" Version="701" BinaryVersion="1">
  <URL Path="$NVIDIA/" Priority="1" />
</Info>
EOF

# --- Market billing v2 is gone; bindService with an implicit intent throws on API 21+
sed -i 's#\.catch Ljava/lang/SecurityException; {:try_start_0 \.\. :try_end_0} :catch_0#.catch Ljava/lang/Exception; {:try_start_0 .. :try_end_0} :catch_0#' \
  "$SMALI/BillingService.smali"

# --- NVIDIA's CDN now answers chunked (no Content-Length) and HttpDownload treats size -1 as an error.
# BufferedHttpEntity reads such responses into memory (pieces are <= 5 MB) and reports the real length.
perl -0pi -e 's#(getEntity\(\)Lorg/apache/http/HttpEntity;\n\n    move-result-object v2\n)#$1\n    new-instance v3, Lorg/apache/http/entity/BufferedHttpEntity;\n    invoke-direct {v3, v2}, Lorg/apache/http/entity/BufferedHttpEntity;-><init>(Lorg/apache/http/HttpEntity;)V\n    invoke-interface {v1, v3}, Lorg/apache/http/HttpResponse;->setEntity(Lorg/apache/http/HttpEntity;)V\n    move-object v2, v3\n#' \
  "$SMALI/HttpDownload.smali"
grep -q BufferedHttpEntity "$SMALI/HttpDownload.smali"

# --- EGL config pick: the game scores configs by distance to RGB565 but starts from a threshold that
# only an exact 565 config can beat. Modern GPUs (Adreno 7xx/8xx) offer no 565 configs, so nothing
# matched ("OpenGL initialization failed"). Start from INT_MAX so the closest config always wins.
perl -0pi -e 's#(\.method public JavaCallback_initEGL\(.*?)const v6, 0xf4240#$1const v6, 0x7fffffff#s' "$SMALI/ddapp.smali"
grep -q "const v6, 0x7fffffff" "$SMALI/ddapp.smali"

# --- text input (hero / game names): the field was multi-line, so a touch keyboard's Enter typed a
# newline, and the only accept path was tapping the game's button, which the keyboard covers on
# tablets; Back (closing the keyboard) discarded the text. Single-line + "Done" accepts, and so does
# closing the keyboard.
cat > "$SMALI/EditTextBackEvent.smali" <<'SMALI'
.class final Lcom/trendy/ddapp/EditTextBackEvent;
.super Landroid/widget/EditText;
.implements Landroid/widget/TextView$OnEditorActionListener;

.field private OurActivity:Lcom/trendy/ddapp/ddapp;

.method public constructor <init>(Landroid/content/Context;)V
    .locals 1
    invoke-direct {p0, p1}, Landroid/widget/EditText;-><init>(Landroid/content/Context;)V
    check-cast p1, Lcom/trendy/ddapp/ddapp;
    iput-object p1, p0, Lcom/trendy/ddapp/EditTextBackEvent;->OurActivity:Lcom/trendy/ddapp/ddapp;
    const/4 v0, 0x1
    invoke-virtual {p0, v0}, Lcom/trendy/ddapp/EditTextBackEvent;->setSingleLine(Z)V
    const v0, 0x10000006    # IME_ACTION_DONE | IME_FLAG_NO_EXTRACT_UI
    invoke-virtual {p0, v0}, Lcom/trendy/ddapp/EditTextBackEvent;->setImeOptions(I)V
    invoke-virtual {p0, p0}, Lcom/trendy/ddapp/EditTextBackEvent;->setOnEditorActionListener(Landroid/widget/TextView$OnEditorActionListener;)V
    return-void
.end method

.method public final onEditorAction(Landroid/widget/TextView;ILandroid/view/KeyEvent;)Z
    .locals 2
    iget-object v0, p0, Lcom/trendy/ddapp/EditTextBackEvent;->OurActivity:Lcom/trendy/ddapp/ddapp;
    const/4 v1, 0x0
    invoke-virtual {v0, v1}, Lcom/trendy/ddapp/ddapp;->JavaCallback_HideKeyBoard(Z)V
    const/4 v0, 0x1
    return v0
.end method

.method public final onKeyPreIme(ILandroid/view/KeyEvent;)Z
    .locals 2
    const/4 v0, 0x4
    if-ne p1, v0, :not_back
    invoke-virtual {p2}, Landroid/view/KeyEvent;->getAction()I
    move-result v0
    const/4 v1, 0x1
    if-ne v0, v1, :consume
    iget-object v0, p0, Lcom/trendy/ddapp/EditTextBackEvent;->OurActivity:Lcom/trendy/ddapp/ddapp;
    const/4 v1, 0x0
    invoke-virtual {v0, v1}, Lcom/trendy/ddapp/ddapp;->JavaCallback_HideKeyBoard(Z)V
    :consume
    const/4 v0, 0x1
    return v0
    :not_back
    invoke-super {p0, p1, p2}, Landroid/widget/EditText;->onKeyPreIme(ILandroid/view/KeyEvent;)Z
    move-result v0
    return v0
.end method
SMALI

# --- launcher icon: modern launchers shrink the 48px legacy icon into a white plate. Android 8+
# picks this adaptive icon instead; it reuses the game's own 170px art (drawn for the Xperia Play),
# enlarged just enough that its baked-in frame falls outside the launcher mask.
mkdir -p "$S/res/drawable-anydpi-v26"
cat > "$S/res/drawable-anydpi-v26/icon.xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background><color android:color="#FF00300E"/></background>
    <foreground><inset android:drawable="@drawable/xperiaicon" android:inset="10%"/></foreground>
</adaptive-icon>
XML

# --- render at full resolution (the engine asks for ~50% to spare 2012 GPUs), hide the nav bar and
# ask for the panel's 144 Hz mode (ignored where unsupported; the loader lifts the engine's 60 fps cap)
perl -0pi -e 's#(\.method public JavaCallback_SetFixedSizeScale\(F\)V\n    \.locals 2\n\n    \.prologue\n)#$1    const/high16 p1, 0x3f800000\n#' \
  "$SMALI/ddapp.smali"
perl -0pi -e 's#(\.method public onWindowFocusChanged\(Z\)V\n    \.locals 4\n\n    \.prologue\n)#$1    invoke-virtual {p0}, Lcom/trendy/ddapp/ddapp;->getWindow()Landroid/view/Window;\n    move-result-object v0\n    invoke-virtual {v0}, Landroid/view/Window;->getAttributes()Landroid/view/WindowManager\$LayoutParams;\n    move-result-object v1\n    const/high16 v2, 0x43100000\n    iput v2, v1, Landroid/view/WindowManager\$LayoutParams;->preferredRefreshRate:F\n    invoke-virtual {v0, v1}, Landroid/view/Window;->setAttributes(Landroid/view/WindowManager\$LayoutParams;)V\n    invoke-virtual {v0}, Landroid/view/Window;->getDecorView()Landroid/view/View;\n    move-result-object v0\n    const/16 v1, 0x1706\n    invoke-virtual {v0, v1}, Landroid/view/View;->setSystemUiVisibility(I)V\n#' \
  "$SMALI/ddapp.smali"
grep -q "const/high16 p1, 0x3f800000" "$SMALI/ddapp.smali" && grep -q "const/16 v1, 0x1706" "$SMALI/ddapp.smali"

# --- LAN games are found by UDP broadcast on port 14001; many phones drop broadcast/multicast
# Wi-Fi packets unless the app holds a MulticastLock
sed -i 's#<uses-permission android:name="android.permission.INTERNET"/>#&<uses-permission android:name="android.permission.CHANGE_WIFI_MULTICAST_STATE"/>#' "$S/AndroidManifest.xml"
perl -0pi -e 's#(\.method public onCreate\(Landroid/os/Bundle;\)V\n    \.locals 3\n(?:.*\n)*?    invoke-super \{p0, p1\}, Landroid/app/Activity;->onCreate\(Landroid/os/Bundle;\)V\n)#$1    invoke-virtual {p0}, Lcom/trendy/ddapp/ddapp;->getApplicationContext()Landroid/content/Context;\n    move-result-object v0\n    const-string v1, "wifi"\n    invoke-virtual {v0, v1}, Landroid/content/Context;->getSystemService(Ljava/lang/String;)Ljava/lang/Object;\n    move-result-object v0\n    check-cast v0, Landroid/net/wifi/WifiManager;\n    if-eqz v0, :ddport_no_wifi\n    const-string v1, "dundef-lan"\n    invoke-virtual {v0, v1}, Landroid/net/wifi/WifiManager;->createMulticastLock(Ljava/lang/String;)Landroid/net/wifi/WifiManager\$MulticastLock;\n    move-result-object v0\n    invoke-virtual {v0}, Landroid/net/wifi/WifiManager\$MulticastLock;->acquire()V\n    :ddport_no_wifi\n#' \
  "$SMALI/ddapp.smali"
grep -q "dundef-lan" "$SMALI/ddapp.smali" && grep -q CHANGE_WIFI_MULTICAST_STATE "$S/AndroidManifest.xml"

# --- native code: our 64-bit loader takes the game's lib name; the untouched 32-bit lib rides along
GUEST=$S/lib/armeabi/libDunDef-Android.so
for abi in $ABIS; do
  B=port/build-$abi
  [ -f "$B/build.ninja" ] || cmake -S port -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static >/dev/null
  ninja --quiet -C "$B" DunDef-Android
  mkdir -p "$S/lib/$abi"
  "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip -o "$S/lib/$abi/libDunDef-Android.so" "$B/libDunDef-Android.so"
  cp "$GUEST" "$S/lib/$abi/libDunDefGuest.so"
done
rm -r "$S/lib/armeabi" "$S/unknown/com/trendy/ddapp/ddapp.java.bak"

apktool b -q -o "$W/unsigned.apk" "$S"
"$BT/zipalign" -f -p 4 "$W/unsigned.apk" "$W/aligned.apk"

[ -f dundef.keystore ] || keytool -genkeypair -keystore dundef.keystore -storepass dundef123 \
  -keypass dundef123 -alias dundef -keyalg RSA -keysize 2048 -validity 36500 \
  -dname "CN=DunDef SW Port" >/dev/null
"$BT/apksigner" sign --ks dundef.keystore --ks-pass pass:dundef123 --ks-key-alias dundef \
  --out "$OUT" "$W/aligned.apk"
"$BT/apksigner" verify "$OUT" && echo "OK -> $OUT"
