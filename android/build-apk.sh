#!/usr/bin/env bash
# Compila l'APK senza Gradle (aapt + javac + dx/d8 + apksigner).
# Uso: ANDROID_JAR=/percorso/android.jar ./build-apk.sh
set -euo pipefail
cd "$(dirname "$0")"
ANDROID_JAR="${ANDROID_JAR:-/usr/lib/android-sdk/platforms/android-23/android.jar}"
OUT=build
rm -rf "$OUT"; mkdir -p "$OUT/classes" "$OUT/dex"
aapt package -f -M AndroidManifest.xml -S res -I "$ANDROID_JAR" -F "$OUT/base.apk" \
  --min-sdk-version 23 --target-sdk-version 34
javac -nowarn -Xlint:-options -source 8 -target 8 -encoding UTF-8 -bootclasspath "$ANDROID_JAR" -d "$OUT/classes" $(find src -name '*.java')
if command -v d8 >/dev/null; then
  d8 --release --min-api 23 --lib "$ANDROID_JAR" --output "$OUT/dex" $(find "$OUT/classes" -name '*.class')
else
  dalvik-exchange --dex --min-sdk-version=23 --output="$OUT/dex/classes.dex" "$OUT/classes"
fi
cp "$OUT/base.apk" "$OUT/unsigned.apk"
(cd "$OUT/dex" && zip -q -j ../unsigned.apk classes.dex)
zipalign -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
KS="${KEYSTORE:-$(pwd)/multiscreen.keystore}"
if [ ! -f "$KS" ]; then
  mkdir -p "$(dirname "$KS")"
  keytool -genkeypair -keystore "$KS" -storepass multiscreen -keypass multiscreen -alias multiscreen \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=MultiScreen" >/dev/null
fi
apksigner sign --ks "$KS" --ks-pass pass:multiscreen --key-pass pass:multiscreen --ks-key-alias multiscreen \
  --out multiscreen.apk "$OUT/aligned.apk"
apksigner verify multiscreen.apk && echo "OK -> $(pwd)/multiscreen.apk"
