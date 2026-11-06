#!/bin/bash
# remove-esp.sh — undo install-esp.sh (delete boot entry + ESP copy)
set -euo pipefail
LABEL='TU102-UNLOCK'
ID=$(efibootmgr | awk -v L="$LABEL" '$NF==L{gsub(/Boot|\*.*/,"",$1); print $1}' | head -1)
if [ -n "$ID" ]; then
    efibootmgr -q -b "$ID" -B && echo "deleted Boot$ID"
else
    echo "no '$LABEL' boot entry found"
fi
rm -rf /boot/efi/EFI/TU102 && echo "removed /boot/efi/EFI/TU102"
efibootmgr | grep -E '^BootOrder|^BootNext' || true
