#!/bin/bash
set -e

if ! grep -q "overlayroot" /boot/firmware/cmdline.txt 2>/dev/null; then
    echo "OverlayFS déjà inactif."
    exit 0
fi

sudo raspi-config nonint do_overlayfs 1
echo "OverlayFS désactivé. Redémarrage..."
sudo reboot
