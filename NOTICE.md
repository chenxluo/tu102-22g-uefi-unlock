# NOTICE — provenance, attribution and boundaries

## What this repository is

A Windows-usable, volatile, pre-OS tensor-rate unlock for the RTX 2080 Ti
22 GiB VRAM-modded card (`10de:1e04`), ported from the CMP 50HX EFI unlocker
and validated end-to-end on real hardware (3/3 successful unlocks, measured
+51–75% FP16/FP32-accumulate GEMM throughput, bit-correct numerics vs an
FP32 reference).

## Code provenance

- `efi-unlock-tu102/unlock50x_v1.c` derives from
  [xrip/cmp50hx-unlock](https://github.com/xrip/cmp50hx-unlock)
  `efi-unlock/unlock50x_v1.c` at commit
  `df2fe11b370bb52cc314576d0f2f5ae47ce908f8`, which itself ports
  [PZH1gdmu/CMP40HX-Unlock](https://github.com/PZH1gdmu/CMP40HX-Unlock) v3.0.0
  (MIT). The inherited MIT notice is kept at
  `efi-unlock-tu102/LICENSE-40HX-UNLOCK`.
- `efi-unlock-tu102/extract_fwsec.py` is taken unchanged from the same
  upstream commit (MIT).
- Local modifications are documented inline and in
  `efi-unlock-tu102/PORT-NOTES.md` (device-id retarget 1e09→1e04, 22 GiB
  geometry, native-FWSEC integration, the four chainload/self-detection
  fixes, identity guards).

## Reference works (documentation only — no code copied)

- [KohakuBlueleaf/KohakUnlock](https://github.com/KohakuBlueleaf/KohakUnlock)
  (GPL-2.0): the published 22 GiB layout constants (fbSize 0x580000000, FRTS
  0x57FE00000, WPR2 0x057FE000/0x057FEE00, 107 MiB GSP heap, capacity
  register 0x2c9) and the engineering methodology were read from its public
  documentation. No source files were copied into this repository; the
  constants above are hardware facts independently confirmed on the porting
  machine (the card's own FWSEC extraction turned out to be byte-identical,
  sha256 c6fa28fa…, to the one documented there).
- [haohervchb/Turing-unlock](https://github.com/haohervchb/Turing-unlock)
  (copyleft-next 0.3.1): cross-reference for the GeForce-Turing EFI approach.
- [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock): Ampere
  mechanism reference; no code shared.
- Jon Pry, *A Canary in the Crypto Mine* (V67 canary research,
  DOI 10.5281/zenodo.20916112): the underlying vulnerability research.

## Firmware redistribution boundary

`blobs/*.bin`, extracted FWSEC images, compiled `*.EFI` files, PROM dumps,
NVRAM backups and run logs are **never** committed (see `.gitignore`). Users
fetch the pinned upstream blobs themselves at build time (hash-verified) and
produce their own FWSEC extraction from their own card.

## No universal unlocker

The shipped code is profiled for one card family and guarded accordingly
(capacity register must read 0x2c9; input hashes are enforced). Porting to
other Turing variants requires a new, separately reviewed profile — do not
weaken the guards to make another card "fit".
