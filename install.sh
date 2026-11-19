#!/usr/bin/env bash
# MultiScreen - installazione su Linux con KDE Plasma (CachyOS/Arch, Kubuntu/Debian, Fedora, openSUSE)
# Uso:  ./install.sh        (dalla cartella scaricata)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$ROOT/logs" 2>/dev/null && exec > >(tee "$ROOT/logs/install.log") 2>&1

B='\e[1m'; G='\e[32m'; Y='\e[33m'; R='\e[31m'; N='\e[0m'
ok()   { printf "${G}✔${N} %s\n" "$*"; }
warn() { printf "${Y}!${N} %s\n" "$*"; }
step() { printf "\n${B}%s${N}\n" "$*"; }
die()  { printf "${R}✘ %s${N}\n" "$*"; command -v kdialog >/dev/null && kdialog --title MultiScreen --error "$*" 2>/dev/null; exit 1; }
as_root() { if [ -t 0 ] || [ -r /dev/tty ]; then sudo "$@"; else pkexec "$@"; fi; }

BIN_DIR="$HOME/.local/bin"
DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/multiscreen"
APP_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
MENU_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/kio/servicemenus"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
BIN="$BIN_DIR/multiscreen-host"
PREBUILT="$ROOT/prebuilt/linux-$(uname -m)/multiscreen-host"

printf "${B}MultiScreen${N} — il tablet diventa un secondo schermo via USB\n"

step "1/5  Controllo il computer"
echo "  $(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-Linux}"), sessione ${XDG_SESSION_TYPE:-?}, desktop ${XDG_CURRENT_DESKTOP:-?}"
[ "${XDG_SESSION_TYPE:-}" = wayland ] || warn "Serve una sessione Wayland (al login scegli «Plasma (Wayland)»)."
case "${XDG_CURRENT_DESKTOP:-}" in *KDE*) ok "KDE Plasma" ;; *) warn "Per ora funziona solo con KDE Plasma." ;; esac

# serve compilare? (solo se il programma gia' pronto non parte su questo sistema)
NEED_BUILD=1
if [ -x "$PREBUILT" ] && "$PREBUILT" --help >/dev/null 2>&1; then NEED_BUILD=0; fi

step "2/5  Installo quello che serve (puo' chiedere la password)"
intel=0; lspci 2>/dev/null | grep -Eiq '(vga|display).*intel' && intel=1
if command -v pacman >/dev/null; then
  RUN="android-tools android-udev gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-plugins-bad-libs gst-plugin-pipewire gst-plugin-va gst-plugins-ugly libva-utils wayland libnotify wl-clipboard xdg-utils"
  DEV="gcc make pkgconf wayland-protocols plasma-wayland-protocols"
  [ $intel = 1 ] && RUN="$RUN intel-media-driver"
  have() { pacman -Qq "$1" >/dev/null 2>&1; }; avail() { pacman -Si "$1" >/dev/null 2>&1; }
  inst() { as_root pacman -S --needed --noconfirm "$@"; }
elif command -v apt-get >/dev/null; then
  RUN="adb gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-pipewire libnotify-bin wl-clipboard xdg-utils vainfo"
  DEV="build-essential pkg-config libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libwayland-dev wayland-protocols plasma-wayland-protocols"
  [ $intel = 1 ] && RUN="$RUN intel-media-va-driver-non-free"
  have() { dpkg -s "$1" >/dev/null 2>&1; }; avail() { apt-cache show "$1" >/dev/null 2>&1; }
  inst() { as_root apt-get install -y "$@"; }
elif command -v dnf >/dev/null; then
  RUN="android-tools gstreamer1-plugins-base gstreamer1-plugins-good gstreamer1-plugins-bad-free gstreamer1-plugins-ugly-free pipewire-gstreamer libnotify wl-clipboard xdg-utils libva-utils"
  DEV="gcc make pkgconf-pkg-config gstreamer1-devel gstreamer1-plugins-base-devel wayland-devel wayland-protocols-devel plasma-wayland-protocols-devel"
  [ $intel = 1 ] && RUN="$RUN intel-media-driver"
  have() { rpm -q "$1" >/dev/null 2>&1; }; avail() { dnf info "$1" >/dev/null 2>&1; }
  inst() { as_root dnf install -y "$@"; }
elif command -v zypper >/dev/null; then
  RUN="android-tools gstreamer-plugins-base gstreamer-plugins-good gstreamer-plugins-bad gstreamer-plugins-ugly gstreamer-plugin-pipewire libnotify-tools wl-clipboard xdg-utils libva-utils"
  DEV="gcc make pkgconf gstreamer-devel gstreamer-plugins-base-devel wayland-devel wayland-protocols-devel plasma-wayland-protocols-devel"
  [ $intel = 1 ] && RUN="$RUN intel-media-driver"
  have() { rpm -q "$1" >/dev/null 2>&1; }; avail() { zypper -q info "$1" 2>/dev/null | grep -q "^Name"; }
  inst() { as_root zypper -n install "$@"; }
else
  die "Distribuzione non riconosciuta: installa a mano adb, gstreamer (pipewire, va, bad, ugly), wl-clipboard."
fi
WANT="$RUN"; [ $NEED_BUILD = 1 ] && WANT="$RUN $DEV"
NEED=""
for p in $WANT; do have "$p" || { avail "$p" && NEED="$NEED $p"; }; done
if [ -n "$NEED" ]; then
  echo "  installo:$NEED"
  inst $NEED || die "Non sono riuscito a installare i pacchetti (password sbagliata o niente internet?)"
fi
ok "tutto il necessario e' installato"

step "3/5  Preparo il programma"
systemctl --user stop multiscreen.service 2>/dev/null
mkdir -p "$BIN_DIR" "$DATA_DIR" "$APP_DIR" "$MENU_DIR" "$UNIT_DIR"
if [ $NEED_BUILD = 1 ]; then
  echo "  compilo (una volta sola)…"
  make -C "$ROOT/linux" clean >/dev/null 2>&1
  make -C "$ROOT/linux" >/dev/null || die "Compilazione non riuscita: guarda logs/install.log"
  install -m 755 "$ROOT/linux/build/multiscreen-host" "$BIN"
else
  install -m 755 "$PREBUILT" "$BIN"
fi
install -m 755 "$ROOT/linux/data/multiscreen" "$BIN_DIR/multiscreen"
install -m 755 "$ROOT/uninstall.sh" "$DATA_DIR/uninstall.sh"
APK="$ROOT/prebuilt/multiscreen.apk"
[ -f "$ROOT/android/multiscreen.apk" ] && [ "$ROOT/android/multiscreen.apk" -nt "$APK" ] && APK="$ROOT/android/multiscreen.apk"
install -m 644 "$APK" "$DATA_DIR/multiscreen.apk"
grep -o 'android:versionCode="[0-9]*"' "$ROOT/android/AndroidManifest.xml" | grep -o '[0-9]*' > "$DATA_DIR/apk-version"
REAL_BIN="$(readlink -f "$BIN")"
sed "s|@BIN@|$REAL_BIN|" "$ROOT/linux/data/com.zago.multiscreen.desktop" > "$APP_DIR/com.zago.multiscreen.desktop"
sed "s|@BIN@|$REAL_BIN|" "$ROOT/linux/data/multiscreen.service" > "$UNIT_DIR/multiscreen.service"
sed "s|@BIN@|$REAL_BIN|" "$ROOT/linux/data/multiscreen-send.desktop" > "$MENU_DIR/multiscreen-send.desktop"
chmod +x "$MENU_DIR/multiscreen-send.desktop"   # Plasma 6 vuole i service menu eseguibili
for k in kbuildsycoca6 kbuildsycoca5; do command -v $k >/dev/null && { $k >/dev/null 2>&1; break; }; done
ok "programma pronto"

step "4/5  Prova"
CHECK_OK=0
for i in 1 2 3 4 5; do
  MULTISCREEN_LOG="$ROOT/logs/check.log" "$BIN" --check >/dev/null 2>&1 && { CHECK_OK=1; break; }
  sleep 1
done
if [ $CHECK_OK = 1 ]; then
  ok "KDE ha dato i permessi"
  echo "  (per 5 secondi compare e sparisce uno schermo di prova)"
  if MULTISCREEN_LOG="$ROOT/logs/selftest.log" "$BIN" --selftest >/dev/null 2>&1; then ok "schermo e video funzionano"
  else warn "la prova video non e' riuscita: guarda logs/selftest.log"; fi
else
  warn "KDE non ha ancora dato i permessi: esci e rientra nella sessione (logout/login) e rilancia l'installazione"
fi

step "5/5  Avvio automatico"
systemctl --user daemon-reload
systemctl --user enable --now multiscreen.service >/dev/null 2>&1 && ok "MultiScreen parte da solo a ogni accensione"
adb start-server >/dev/null 2>&1

MSG="Fatto!

Sul tablet (solo la prima volta):
 1. Impostazioni → Info tablet → tocca 7 volte «Versione»
 2. Opzioni sviluppatore → attiva «Debug USB» (e «Installa tramite USB»)
 3. Collega il cavo e tocca «Consenti» (spunta «Consenti sempre»)

Da quel momento: colleghi il cavo e il secondo schermo compare da solo."
printf "\n${G}%s${N}\n\nComandi: multiscreen status | log | send FILE | restart | off\n" "$MSG"
command -v kdialog >/dev/null && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && kdialog --title MultiScreen --msgbox "$MSG" 2>/dev/null &
exit 0
