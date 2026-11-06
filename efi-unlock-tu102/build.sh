#!/bin/bash
# build.sh — TU102 (RTX 2080 Ti 22G mod) UEFI tensor-unlock build (Linux, gnu-efi)
#
# Prerequisites: sudo apt install build-essential gnu-efi
#
# Two modes:
#   DUMP_ONLY=1 ./build.sh   -> TU102DUMP.EFI  (bootstrap: needs NO firmware
#                                inputs; boots from USB, dumps the card's
#                                1 MiB PROM windows read-only, chainloads OS)
#   ./build.sh               -> TU102UNLK.EFI  (the unlock; REQUIRES the four
#                                TU102 blobs from cmp50hx-unlock@df2fe11 —
#                                run tools/get-blobs.sh — plus YOUR card's
#                                fwsec_native.bin, both hash-verified here)
#
# Outputs are NOT redistributable: they embed NVIDIA firmware images.
set -euo pipefail
cd "$(dirname "$0")"

EFI_INC="${EFI_INC:-/usr/include/efi}"
EFI_LIB="${EFI_LIB:-/usr/lib}"
if [ ! -d "$EFI_INC" ]; then
    echo "gnu-efi headers not found at $EFI_INC (sudo apt install build-essential gnu-efi)" >&2
    exit 1
fi

SRC=unlock50x_v1.c
OBJ=tu102.o
OUT=tu102.so
EFIOUT=TU102UNLK.EFI
EXTRA_DEFS=""
BASE_DEFS="-DGNU_EFI_USE_MS_ABI -DDIRECT_SEC2 -DRELEASE_BUILD"

# ---- expected hashes -------------------------------------------------------
# The four TU102 blobs pinned to xrip/cmp50hx-unlock @ df2fe11
# (efi-unlock/blobs/), cross-checked against KohakUnlock's
# profiles/leadtek-tu102-22g.json:
BL_V67=de7288bcfb80c2d4b259a59e823c926ba0a9adcab594219cc6802c94257ce271
BL_BOOTER=e0f0fc93da097b5b68e5af3adb58b2235ed1fad5fce0b23592d3895af44cb743
BL_GSPRM=fa43239bcee7b97ca62f007cc68487560a39e19f74f3dde7486db3f98df8e471
BL_BLGSP=f21f1cfbb8fffa8067685314367fc4c50f3f84dd99eda7c19db4292d9269b9d9
# The FWSEC of the tested 22 GiB mod family (two independent cards matched):
FWSEC_NATIVE=c6fa28fa2458f7bce2aeb422a2e57cce3885ff65c779e1077f682c23d097a635

sha() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }

mkdir -p blobs
DUMP=0
[ "${DUMP_ONLY:-0}" = "1" ] && DUMP=1

if [ "$DUMP" = "1" ]; then
    # Bootstrap build: the dump path never executes any embedded firmware, so
    # missing blobs are replaced by 1-byte placeholders purely to satisfy the
    # linker. The binary is still NOT for redistribution.
    EFIOUT=TU102DUMP.EFI
    EXTRA_DEFS="-DDUMP_ONLY"
    echo "=== DUMP-ONLY build (no firmware inputs required) ==="
else
    need() { [ -f "blobs/$1" ] || { echo "missing blobs/$1" >&2
        echo "  run tools/get-blobs.sh  (TU102 blobs) and extract_fwsec.py (your FWSEC) — see README" >&2
        exit 1; }; }
    need v67_payload.bin; need booter_ucode_prod.bin
    need gsp_rm_boot_dbg.bin; need bl_gsp_tu102.bin; need fwsec_native.bin
    for pair in "v67_payload.bin:$BL_V67" "booter_ucode_prod.bin:$BL_BOOTER" \
                "gsp_rm_boot_dbg.bin:$BL_GSPRM" "bl_gsp_tu102.bin:$BL_BLGSP"; do
        f=${pair%%:*}; exp=${pair##*:}
        if [ "$(sha "blobs/$f")" != "$exp" ]; then
            echo "HASH MISMATCH: blobs/$f != cmp50hx-unlock@df2fe11 expected value" >&2
            exit 1
        fi
    done
    if [ "$(sha blobs/fwsec_native.bin)" != "$FWSEC_NATIVE" ] && [ "${TU102_ALLOW_FWSEC:-0}" != "1" ]; then
        echo "fwsec_native.bin does not match the tested 22 GiB profile (c6fa28fa...)." >&2
        echo "If this is your own dump for a different card: review the port notes," >&2
        echo "adjust the geometry constants, and opt in with TU102_ALLOW_FWSEC=1." >&2
        exit 1
    fi
    # 2280ti server port: GPU0 (10de:1e07) FWSEC is OPTIONAL — from its own
    # PROM dump via extract_fwsec.py. Present without a pinned hash requires
    # an explicit opt-in (self-vouch); pin it here once verified:
    #   TU102_G0_SHA=<sha256>  (or edit FWSEC_G0 below after the dump session)
    FWSEC_G0_DEFAULT_SHA=""   # fill after GPU0's dump is verified
    if [ -f blobs/fwsec_g0.bin ] && [ "$(wc -c < blobs/fwsec_g0.bin)" -gt 1 ]; then
        G0_OK=0
        [ -n "$FWSEC_G0_DEFAULT_SHA" ] && [ "$(sha blobs/fwsec_g0.bin)" = "$FWSEC_G0_DEFAULT_SHA" ] && G0_OK=1
        [ "${TU102_G0_SHA:-}" != "" ] && [ "$(sha blobs/fwsec_g0.bin)" = "$TU102_G0_SHA" ] && G0_OK=1
        if [ "$G0_OK" != "1" ] && [ "${TU102_ALLOW_FWSEC:-0}" != "1" ]; then
            echo "fwsec_g0.bin present but not pinned/verified (set TU102_G0_SHA or TU102_ALLOW_FWSEC=1)" >&2
            exit 1
        fi
        echo "fwsec_g0.bin: $( [ "$G0_OK" = 1 ] && echo hash-verified || echo self-vouched )"
    fi
    echo "=== UNLOCK build (inputs hash-verified) ==="
    # VBIOS_DUMP: the (fixed) upstream PROM dumper also runs during unlock boots
    # and refreshes \50hx_vbios.bin on the boot medium — matches the tested config.
    BASE_DEFS="$BASE_DEFS -DVBIOS_DUMP"
fi

# Symbols still referenced by unused code paths get 1-byte placeholders when
# the real file is absent (never executed; keeps the link whole).
for f in booter_ucode_dbg.bin fwsec_50hx_prod.bin fwsec_50hx_dbg.bin \
         sec2_ucode_vbios_49.bin sec2_ucode_vbios_89.bin; do
    [ -f "blobs/$f" ] || ( cd blobs && printf '\0' > "$f" )
done
# 2280ti port: GPU0 FWSEC placeholder when absent (size gate skips the card)
[ -f "blobs/fwsec_g0.bin" ] || ( cd blobs && printf '\0' > fwsec_g0.bin )
# dump build: every absent blob becomes a placeholder too
if [ "$DUMP" = "1" ]; then
    for f in v67_payload.bin booter_ucode_prod.bin gsp_rm_boot_dbg.bin \
             bl_gsp_tu102.bin fwsec_native.bin; do
        [ -f "blobs/$f" ] || ( cd blobs && printf '\0' > "$f" )
    done
fi
# 2280ti port: real blob sizes for the runtime per-card size gate
sz() { if [ -f "blobs/$1" ]; then wc -c < "blobs/$1" | tr -d ' \n'; else echo 0; fi; }
printf '#define FWSEC_G1_SIZE %s\n#define FWSEC_G0_SIZE %s\n' \
    "$(sz fwsec_native.bin)" "$(sz fwsec_g0.bin)" > blob_sizes.h
echo "blob_sizes.h: G1=$(sz fwsec_native.bin)B G0=$(sz fwsec_g0.bin)B (39968 = full blob)"

echo "=== 1. compile $SRC ==="
gcc -c -O2 -fno-stack-protector -fpic -ffreestanding \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -fshort-wchar -mno-red-zone -maccumulate-outgoing-args \
    -fno-builtin -fno-strict-aliasing -Wno-unused-function \
    -I "$EFI_INC" -I "$EFI_INC/x86_64" \
    $BASE_DEFS $EXTRA_DEFS \
    -o "$OBJ" "$SRC"

echo "=== 2. embed blobs ==="
# Debian/Ubuntu gnu-efi archives hold ELF objects, so the whole link is ELF
# (crt0 + elf_x86_64_efi.lds) and the blob objects must be ELF too.
# objcopy derives the symbol name from the FILENAME AS GIVEN — run it from
# blobs/ with a bare name (upstream lesson: prefixed symbols redefined to
# zero and every falcon ran zero-filled code).
embed() {
  local f="$1" tgt="$2"
  local base="${f//./_}"
  ( cd blobs && objcopy --input-target binary --output-target elf64-x86-64 \
      --binary-architecture i386:x86-64 \
      --redefine-sym "_binary_${base}_start=${tgt}" \
      --redefine-sym "_binary_${base}_end=${tgt}_end" \
      --redefine-sym "_binary_${base}_size=${tgt}_size" \
      "$f" "../${tgt}.o" )
  if ! nm "${tgt}.o" | grep -qE "^[0-9a-f]+ [A-Za-z] ${tgt}$"; then
    echo "  EMBED FAILED for $f (symbol ${tgt} not defined)" >&2
    exit 1
  fi
  echo "  embedded $f -> ${tgt}"
}
embed v67_payload.bin         v67_payload_bin
embed booter_ucode_dbg.bin    booter_ucode_dbg
embed booter_ucode_prod.bin   booter_ucode_prod
embed gsp_rm_boot_dbg.bin     gsp_rm_boot_dbg
embed fwsec_50hx_prod.bin     fwsec_50hx_prod_bin
embed fwsec_native.bin        fwsec_native_bin
embed fwsec_g0.bin            fwsec_g0_bin
embed fwsec_50hx_dbg.bin      fwsec_50hx_dbg_bin
embed sec2_ucode_vbios_49.bin sec2_ucode_vbios_49
embed sec2_ucode_vbios_89.bin sec2_ucode_vbios_89
embed bl_gsp_tu102.bin        gsp_bl_tu102

OBJS="$OBJ v67_payload_bin.o booter_ucode_dbg.o booter_ucode_prod.o \
gsp_rm_boot_dbg.o fwsec_50hx_prod_bin.o fwsec_native_bin.o fwsec_g0_bin.o \
fwsec_50hx_dbg_bin.o sec2_ucode_vbios_49.o sec2_ucode_vbios_89.o gsp_bl_tu102.o"

echo "=== 3. link ==="
ld -nostdlib -znocombreloc -T "$EFI_LIB/elf_x86_64_efi.lds" -shared -Bsymbolic \
    "$EFI_LIB/crt0-efi-x86_64.o" $OBJS \
    -L"$EFI_LIB" -lgnuefi -lefi -o "$OUT" 2>&1 | tail -12

if [ ! -f "$OUT" ]; then
    echo "link failed" >&2
    exit 1
fi

echo "=== 4. convert ==="
objcopy -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym \
    -j .reloc -j .rel -j .rela -j .rel.* -j .rela.* \
    --target=efi-app-x86_64 "$OUT" "$EFIOUT" 2>&1 | tail -4
ls -la "$EFIOUT" && sha256sum "$EFIOUT" && echo BUILD_OK
