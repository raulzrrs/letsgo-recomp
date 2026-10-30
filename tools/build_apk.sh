#!/bin/bash
# tools/build_apk.sh: build/android/LetsGoRecomp.apk from what `make TARGET=android` built (libmain.so, liblasgame.so)
# and make deps-android (SDL3 and its Java side, the C++ runtime). No Gradle: javac + d8 for the Java (SDL's activity and
# ours, android/java), aapt2 for the manifest and icon, zipalign + apksigner with a debug key (build/android/debug.keystore,
# made on first use). Needs the Android SDK (ANDROID_SDK: build-tools 36.1.0, platforms/android-36) and a JDK (JAVA_HOME
# or $ANDROID_SDK/jdk/*).
set -euo pipefail
unset MSYS_NO_PATHCONV  # (its cmd //c calls need Git Bash's argument conversion)
cd "$(dirname "$0")/.."
SDK=${ANDROID_SDK:-$(cygpath -u "${USERPROFILE:-$HOME}")/Android}
BT=$SDK/build-tools/36.1.0
JAR=$SDK/platforms/android-36/android.jar
JDK=${JAVA_HOME:+$(cygpath -u "$JAVA_HOME")}
[ -n "$JDK" ] || JDK=$(ls -d "$SDK"/jdk/*/ 2>/dev/null | head -1)
B=build/android
D=build/deps-android
O=$B/apk
for f in "$BT/aapt2.exe" "$JAR" "$JDK/bin/javac.exe" "$B/libmain.so" "$B/liblasgame.so" "$D/lib/libSDL3.so"; do
    [ -e "$f" ] || { echo "build_apk: missing $f"; exit 1; }
done
w() { cygpath -w "$1"; }
STRIP=${ANDROID_NDK:-$SDK/ndk/29}/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe
rm -rf "$O" && mkdir -p "$O/classes" "$O/lib/arm64-v8a" "$O/res/mipmap-xxxhdpi"

# Java: SDL's org.libsdl.app and dev.letsgorecomp
find "$D/java" android/java -name '*.java' | xargs -n1 cygpath -m > "$O/sources.txt"
"$JDK/bin/javac.exe" -nowarn -Xlint:-options -source 8 -target 8 -bootclasspath "$(w "$JAR")" -d "$(w "$O/classes")" \
    "@$(w "$O/sources.txt")"
python -I - "$O/classes" "$O/classes.jar" <<'EOF'
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[2], 'w') as z:
    for d, _, fs in os.walk(sys.argv[1]):
        for f in fs: z.write(os.path.join(d, f), os.path.relpath(os.path.join(d, f), sys.argv[1]))
EOF
JAVA_HOME=$(w "$JDK") cmd //c "$(w "$BT/d8.bat")" --min-api 29 --lib "$(w "$JAR")" --output "$(w "$O")" "$(w "$O/classes.jar")"

# manifest and icon
cp docs/icon/icon_1024.png "$O/res/mipmap-xxxhdpi/ic_launcher.png"
"$BT/aapt2.exe" compile -o "$(w "$O/res.zip")" "$(w "$O/res/mipmap-xxxhdpi/ic_launcher.png")"
"$BT/aapt2.exe" link -o "$(w "$O/base.apk")" --manifest "$(w android/AndroidManifest.xml)" -I "$(w "$JAR")" "$(w "$O/res.zip")"

# native libraries (stripped: the debug info stays in build/android for crash reports)
"$STRIP" --strip-debug -o "$O/lib/arm64-v8a/libmain.so" "$B/libmain.so"
"$STRIP" --strip-debug -o "$O/lib/arm64-v8a/liblasgame.so" "$B/liblasgame.so"
cp "$D/lib/libSDL3.so" "$D/lib/libc++_shared.so" "$O/lib/arm64-v8a/"
python -I - "$O/base.apk" "$O" <<'EOF'
import os, sys, zipfile
apk, o = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk, 'a', zipfile.ZIP_DEFLATED) as z:
    z.write(os.path.join(o, 'classes.dex'), 'classes.dex')
    for f in sorted(os.listdir(os.path.join(o, 'lib/arm64-v8a'))):
        z.write(os.path.join(o, 'lib/arm64-v8a', f), 'lib/arm64-v8a/' + f)
EOF
"$BT/zipalign.exe" -f -P 16 4 "$(w "$O/base.apk")" "$(w "$O/aligned.apk")"
[ -f "$B/debug.keystore" ] || "$JDK/bin/keytool.exe" -genkeypair -keystore "$(w "$B/debug.keystore")" -storepass android \
    -keypass android -alias debug -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null
JAVA_HOME=$(w "$JDK") cmd //c "$(w "$BT/apksigner.bat")" sign --ks "$(w "$B/debug.keystore")" --ks-pass pass:android \
    --out "$(w "$B/LetsGoRecomp.apk")" "$(w "$O/aligned.apk")"
echo "build_apk: $B/LetsGoRecomp.apk, $(du -h "$B/LetsGoRecomp.apk" | cut -f1)"
