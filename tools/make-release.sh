#!/usr/bin/env bash
# Compila host Linux e app Android e prepara dist/ per una release GitHub.
set -euo pipefail
cd "$(dirname "$0")/.."
make -C linux clean >/dev/null && make -C linux
mkdir -p prebuilt/linux-x86_64 dist
install -m 755 linux/build/multiscreen-host prebuilt/linux-x86_64/multiscreen-host
strip prebuilt/linux-x86_64/multiscreen-host
(cd android && bash build-apk.sh)
cp android/multiscreen.apk prebuilt/multiscreen.apk
rm -f dist/*
tar czf dist/multiscreen-linux.tar.gz --transform 's,^\.,MultiScreen,' \
  --exclude=./.git --exclude=./dist --exclude=./logs --exclude=./linux/build --exclude=./android/build \
  --exclude='*.keystore' --exclude='./android/multiscreen.apk*' --exclude=./.dev --exclude='./.multiscreen-src*' .
cp android/multiscreen.apk dist/MultiScreen.apk
ls -la dist
