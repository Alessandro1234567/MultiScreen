#!/usr/bin/env bash
# Installazione con un comando:
#   curl -fsSL https://raw.githubusercontent.com/Alessandro1234567/multiscreen/main/get.sh | bash
set -euo pipefail
REPO="Alessandro1234567/multiscreen"
DIR="${XDG_DATA_HOME:-$HOME/.local/share}/multiscreen/pacchetto"
TMP="$(mktemp -d)"
echo "Scarico MultiScreen…"
curl -fsSL "https://github.com/$REPO/releases/latest/download/multiscreen-linux.tar.gz" -o "$TMP/ms.tar.gz"
rm -rf "$DIR" && mkdir -p "$DIR"
tar xzf "$TMP/ms.tar.gz" -C "$DIR" --strip-components=1
rm -rf "$TMP"
exec bash "$DIR/install.sh" </dev/tty
