#!/bin/bash
# install-esp.sh — 2280ti server: deploy TU102UNLK.EFI to the internal ESP and
# create a firmware boot entry (Linux port of install-esp.ps1; efibootmgr
# builds the device path itself — no hand-rolled NVRAM writes).
# Run ON THE SERVER. Prereq: sudo apt install efibootmgr; Secure Boot off.
# Usage:
#   sudo bash tools/install-esp.sh            # copy + create entry (appended; ubuntu stays default)
#   sudo bash tools/install-esp.sh --next     # + BootNext one-shot (unlock on next boot only)
#   sudo bash tools/install-esp.sh --first    # + unlock becomes the DEFAULT boot entry
# Undo: sudo bash tools/remove-esp.sh
set -euo pipefail
cd "$(dirname "$0")/.."
LABEL='TU102-UNLOCK'
LOADER='\EFI\TU102\TU102UNLK.EFI'
EFI=efi-unlock-tu102/TU102UNLK.EFI
[ -f "$EFI" ] || { echo "missing $EFI (build first: bash build.sh)"; exit 1; }
command -v efibootmgr >/dev/null || { echo "apt install efibootmgr"; exit 1; }

ESP=$(findmnt -n -o SOURCE /boot/efi)
DISK=$(lsblk -no PKNAME "$ESP" | head -1)
PART=$(lsblk -no PARTN "$ESP" | head -1)
[ -n "$DISK" ] && [ -n "$PART" ] || { echo "cannot resolve ESP disk/part from $ESP"; exit 1; }
echo "ESP $ESP -> /dev/$DISK part$PART"

mkdir -p /boot/efi/EFI/TU102
cp "$EFI" /boot/efi/EFI/TU102/TU102UNLK.EFI
sha256sum /boot/efi/EFI/TU102/TU102UNLK.EFI

mkdir -p local/nvram-backup
efibootmgr -v > "local/nvram-backup/nvram-$(date +%Y%m%d-%H%M%S).txt"
echo "NVRAM backed up to local/nvram-backup/"

entry_id() { efibootmgr | awk -v L="$LABEL" '$NF==L{gsub(/Boot|\*.*/,"",$1); print $1}' | head -1; }
OLD=$(entry_id)
[ -n "$OLD" ] && { efibootmgr -q -b "$OLD" -B; echo "removed stale Boot$OLD"; }

efibootmgr -q --create --disk "/dev/$DISK" --part "$PART" \
    --label "$LABEL" --loader "$LOADER"
NEW=$(entry_id)
[ -n "$NEW" ] || { echo "boot entry creation failed"; exit 1; }
echo "created Boot$NEW ($LABEL), appended to BootOrder end (ubuntu remains default)"

case "${1:-}" in
--next)
    efibootmgr -q -n "$NEW"
    echo "BootNext=Boot$NEW — one-shot unlock on the NEXT boot, then ubuntu again"
    ;;
--first)
    REST=$(efibootmgr | awk '/^BootOrder:/{print $2}' | tr ',' '\n' \
           | grep -v "^$NEW$" | paste -sd,)
    efibootmgr -q -o "$NEW,${REST:-}"
    echo "BootOrder: unlock FIRST — every power-on unlocks, then chainloads GRUB"
    echo "remote switch-back:  sudo efibootmgr -o <ubuntu-num>,$NEW"
    ;;
esac
efibootmgr | grep -E '^BootOrder|^BootNext' || true
