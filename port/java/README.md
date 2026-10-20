# Java side of the port

`com/trendy/ddapp/Gamepad.java` is compiled to smali and committed as
`patches/smali/com/trendy/ddapp/Gamepad.smali`, so `build.py` users don't need a Java compiler.
After editing it, regenerate the smali (Linux/macOS, Android SDK installed):

```bash
SDK=~/Android/Sdk; W=$(mktemp -d)
javac -source 8 -target 8 -cp "$SDK/platforms/android-35/android.jar" -d "$W" \
  stub/com/trendy/ddapp/ddapp.java com/trendy/ddapp/Gamepad.java
"$SDK/build-tools/35.0.0/d8" --min-api 24 --output "$W" "$W"/com/trendy/ddapp/Gamepad.class
(cd "$W" && zip -q g.apk classes.dex)          # apktool disassembles a dex wrapped as an APK
java -jar ../../tools/apktool_2.12.1.jar d -q -f -r -o "$W/out" "$W/g.apk"
cp "$W/out/smali/com/trendy/ddapp/Gamepad.smali" ../../patches/smali/com/trendy/ddapp/
```

`stub/` only declares the members of the game's `ddapp` class that `Gamepad` calls, so it compiles.
