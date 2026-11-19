#!/usr/bin/env bash
systemctl --user disable --now multiscreen.service 2>/dev/null
rm -f ~/.config/systemd/user/multiscreen.service ~/.local/bin/multiscreen-host ~/.local/bin/multiscreen \
      ~/.local/share/applications/com.zago.multiscreen.desktop \
      ~/.local/share/kio/servicemenus/multiscreen-send.desktop
rm -rf ~/.local/share/multiscreen ~/.cache/multiscreen ~/.config/multiscreen
systemctl --user daemon-reload
command -v adb >/dev/null && adb uninstall com.zago.multiscreen 2>/dev/null
echo "MultiScreen rimosso."
