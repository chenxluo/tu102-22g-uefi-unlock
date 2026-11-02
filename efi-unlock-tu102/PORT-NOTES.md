# TU102 2080 Ti 22G port — 变更记录(完整)

本目录是 `xrip/cmp50hx-unlock` `efi-unlock/unlock50x_v1.c`(固定提交
`df2fe11b370bb52cc314576d0f2f5ae47ce908f8`)的移植,目标:RTX 2080 Ti 22 GiB
显存改装卡(`10de:1e04`,VBIOS 家族 90.02.33.00.xx)的 pre-OS 张量解锁,
宿主为 Windows(UEFI,GSP via `EnableGpuFirmware=1`)。

## 许可与来源

- 上游溯源:`PZH1gdmu/CMP40HX-Unlock`(MIT)→ `xrip/cmp50hx-unlock`(TU102 CMP 50HX)
  → 本移植(MIT,`LICENSE-40HX-UNLOCK` 保留于本目录)
- 22 GiB 几何参数的参照:`KohakuBlueleaf/KohakUnlock`(GPL-2.0)**仅参考其公开
  文档中的实测常量,未复制代码**;实测发现本机卡的原生 FWSEC 与其 Leadtek 参照卡
  逐字节一致(sha256 `c6fa28fa…`)
- `blobs/` 下的一切(NVIDIA 固件)与构建产物 `*.EFI` 不入库、不分发

## 变更清单(相对上游 unlock50x_v1.c)

### A. 目标与身份
1. 设备门禁 `0x1E09` → `U40X_TARGET_DID = 0x1E04`(fast/sweep 两处 probe)
2. `efi_main` 增加容量身份守卫:`0x100ce0 != 0x2c9` 直接拒载(22 GiB 布局指纹)

### B. 22 GiB 几何(FW50_* 常量,来源:本机 PROM 解析 + KohakUnlock 文档交叉验证)
- `FW50_CODE_SIZE/DATA_OFF = 0x9800`、`FW50_DMEM_SIZE = 0x420`、
  `FW50_BLOB_SIZE = 0x9C20`(39968 B,与 c6fa28fa FWSEC 一致)
- `FW50_MAPPER_OFF = 0x390`、`FW50_CMDIN_OFF = 0x3E0`、`FW50_SEC_SIZE = 0x9400`
- `FW50_FRTS_OFFSET = 0x57FE00000`、`FW50_WPR2_LO/HI_UP = 0x057FE000/0x057FEE00`
- `g_fbSize = 0x580000000`;`gspFwHeapSize` 表新增 22G 档 `0x6B00000`(107 MiB)
- FWSEC blob 以 `fwsec_native.bin` 名称嵌入并替换 [8b] 步骤引用

### C. 上游缺陷修复(行为级,均有机型实测佐证)
1. `u40x_vbios_dump()` 绝对地址传给 BAR0 相对 `mmio_read32()`(双重加基址,仅特定
   主机碰巧可用)→ 修正为相对偏移,窗口改用本机实测有效的 `BAR0+0x300000`
2. 链载自检 `fsz == g_ourImageSize` 比较的是文件大小 vs 内存 ImageSize(永不
   相等)→ 新增 `g_ourImageBase` 内容比对(前 64 字节),ladder 与递归扫描两处
3. **移除** RAM(SourceBuffer)加载 bootmgfw 的回退路径:bootmgfw 会先
   `ExitBootServices` 再因 SourceBuffer 加载无 DeviceHandle(找不到 BCD)而
   `ABORTED(0x11)`,Boot Services 已死 → 固件被迫整机重 POST → 易失解锁丢失
4. 文件式 `LoadImage(DevicePath)` 的 NOT_FOUND:经 GPT 全盘枚举找到 loader 时
   `FileDevicePath(整盘句柄, path)` 缺 HD(partition) 节点 → 新增按分区起始 LBA
   反查分区句柄、重构完整设备路径的兜底(实测命中后一次 StartImage 成功直通)

### F. 双卡串行解锁与热重启漂移修复
1. **双卡串行调度**: 统一使用方案 B 影驰 310W 公版固件 (`276672.rom`, Subsystem `10de:1e04`),
   按序遍历 Card 0 (主显示) 与 Card 1 (无头副卡), 独立管理 BAR0 窗口与 Falcon 握手
2. **热重启 SS0/SS1 漂移放行**: Windows 驱动与 GSP-RM 运行后动态保留的 SS0/SS1 状态不再
   触发 `0x800000000000001a` 致命拦截, 降级为只读记录并放行, 完整保留 BOOT0、容量守卫与停机门禁
3. **极速张量基准测试**: 提供 `bench/quick_fp_bench.py` 直接调用系统 cuBLAS, 1.5 秒完成
   4096 GEMM Tensor Core 吞吐率判定

### D. 诊断与安全(DUMP_ONLY 构建)
- `DUMP_ONLY=1` 产出零固件依赖的 `TU102DUMP.EFI`:L0-L3 里程碑打印 + 拍照暂停、
  capacity/workspace 预检、双窗口只读转储(`+0x300000` 有效、`+0x11000` 对照)、
  SFS 写回 + BlockIo 裸写双路径,在任何写操作前早退链载 OS
- build.sh:dump 构建对缺失 blob 用 1 字节占位(运行时不触达);unlock 构建对
  四个 TU102 blob 与 `fwsec_native.bin` 强哈希校验(异卡自担风险需显式
  `TU102_ALLOW_FWSEC=1`)

### E. 平台工具(tools/)
- `get-blobs.sh`:从 cmp50hx@df2fe11 取四个 TU102 blob 并校验
- `install-esp.ps1 / remove-esp.ps1`:内置 ESP + 固件 Boot0080 条目方案
  (克隆本机 Windows Boot Manager 设备路径,NVRAM 全量备份/恢复)
- `make-usb.ps1`:U 盘方案(救援/初始 dump)
- `qemu-test.sh`:OVMF 冒烟(无卡也能验证镜像可加载、流程打印正常)

## 实测结论摘要

见 `docs/RESULTS.md`:解锁执行 3/3 成功;FP16/FP32 累加 GEMM +51~75%;
FP32 对照持平;数值正确(2.5e-4);两条引导路径(U 盘 / ESP BootNext)均落地。
