#!/bin/bash
set -e

if grep -q "overlayroot" /boot/firmware/cmdline.txt 2>/dev/null; then
    echo "OverlayFS déjà actif."
    exit 0
fi

sudo raspi-config nonint do_overlayfs 0
echo "OverlayFS activé. Redémarrage..."
sudo reboot
