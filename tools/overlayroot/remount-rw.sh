#!/bin/bash
set -e

if ! grep -q "overlayroot" /boot/firmware/cmdline.txt 2>/dev/null; then
    echo "OverlayFS non actif, le système est déjà en R/W."
    exit 0
fi

sudo mount -o remount,rw /overlay/lower
sudo mount -o remount,rw /boot/firmware
echo "Système remontée en R/W. Les modifications seront permanentes sur la SD."
echo "Redémarrez pour rétablir la protection."
