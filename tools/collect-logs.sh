#!/usr/bin/env bash
# Copia i log di MultiScreen nella cartella logs/ del progetto (per il debug)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; L="$ROOT/logs"; mkdir -p "$L"
S="${XDG_STATE_HOME:-$HOME/.local/state}/multiscreen"
cp -f "$S/host.log" "$L/host.log" 2>/dev/null; cp -f "$S/host.log.1" "$L/host.log.1" 2>/dev/null
journalctl --user -u multiscreen.service -n 300 --no-pager > "$L/journal.txt" 2>&1
T="timeout 8"
{ $T adb devices -l
  if $T adb get-state >/dev/null 2>&1; then
    $T adb shell getprop ro.product.model; $T adb shell wm size; $T adb shell wm density
    $T adb shell dumpsys package com.zago.multiscreen | grep -E 'versionCode|versionName'; $T adb reverse --list
  else echo "nessun tablet collegato via adb"; fi; } > "$L/adb.txt" 2>&1
if $T adb get-state >/dev/null 2>&1; then
  $T adb logcat -d -t 400 -s MultiScreen:V AndroidRuntime:E MediaCodec:W CCodec:W > "$L/tablet-logcat.txt" 2>&1
fi
lsusb > "$L/usb.txt" 2>&1
timeout 5 kscreen-doctor -o > "$L/screens.txt" 2>&1
echo "log copiati in $L"
