#!/bin/bash
# qemu-test.sh — OVMF smoke test (no GPU required, no firmware inputs needed)
# Boots the DUMP_ONLY build under QEMU/OVMF to verify the image loads, prints
# its milestones, writes its log back to the FAT image and exits cleanly.
# Prerequisites: sudo apt install qemu-system-x86 ovmf mtools
set -u
cd "$(dirname "$0")/../efi-unlock-tu102" || exit 1
[ -f TU102DUMP.EFI ] || { echo "build first: DUMP_ONLY=1 bash build.sh"; exit 1; }

TMP=$(mktemp -d)
cp TU102DUMP.EFI "$TMP/"
cd "$TMP"
dd if=/dev/zero of=fat.img bs=1M count=64 status=none
mformat -i fat.img ::
mmd -i fat.img ::/EFI ::/EFI/BOOT
mcopy -i fat.img ./TU102DUMP.EFI ::/EFI/BOOT/BOOTX64.EFI

echo "=== OVMF boot (serial); ~40 s of app pauses included ==="
timeout 150 qemu-system-x86_64 -accel kvm -accel tcg -m 512 \
    -drive file=fat.img,format=raw -display none -serial stdio \
    -bios /usr/share/ovmf/OVMF.fd > serial.log 2>&1
grep -aE 'TU102|APP IS RUNNING|GPU not found|no NVIDIA|=====' serial.log | head -20
if mcopy -i fat.img ::/50hx_log.txt ./ovmf-log.txt 2>/dev/null; then
    echo "--- SFS write-back OK (50hx_log.txt written by the app) ---"
else
    echo "--- no 50hx_log.txt written back ---"
fi
echo "expected result: banner prints, GPU not found (no GPU in QEMU), clean abort"
