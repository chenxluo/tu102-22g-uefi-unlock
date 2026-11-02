# TU102 22G UEFI Unlock(RTX 2080 Ti 22GB 改装卡 · Windows 可用)

针对 **RTX 2080 Ti 22GB 显存改装卡**(`10de:1e04`,国内二手市场常见改装型号)的
**易失性张量算力解锁**——UEFI pre-OS 方案,Windows 直接受益,不刷 VBIOS、不打驱动
补丁、断电即回厂。

**在一张卡上完整落地并实测:+51%~75% FP16/FP32 累加张量吞吐**(见[实测数据](docs/RESULTS.md))。

> ⚠️ **实验性固件级操作,风险自担。** 详见[安全边界](#安全边界)。本仓库不包含任何
> NVIDIA 固件;构建时由使用者自行获取并经哈希校验。

## 这是什么 / 不是什么

NVIDIA 在消费级 Turing 上把 Tensor Core 的 **FP16 输入/FP32 累加(HMMA.F32)** 限制为
半速。本方案在 OS 引导前,通过 cmp50hx-unlock 项目的 V67 漏洞链
(固件签名 canary → 打开 FECS PLM 保护)把速度选择寄存器
**SS0(0x409664)=0x88888888、SS1(0x40966c)=0x8** 写回满速,然后无 POST 直通
Windows,由 `EnableGpuFirmware=1` 驱动模式接受该状态。

- ✅ FP16/FP32 累加张量与 GEMM:约 **1.5~1.75×**
- ➖ FP16/FP16 累加、INT8、FP32、游戏:不变(对照组实测持平)
- ❌ 不增加显存/SM/核心;Turing 无 BF16/TF32 加速,与本方案无关
- ❌ 不适用于 11GB 原装 2080 Ti 或其他型号(容量寄存器守卫会拒载)

## 成果与溯源

| 组件 | 来源 |
|---|---|
| EFI 加载器/漏洞链基座 | [xrip/cmp50hx-unlock](https://github.com/xrip/cmp50hx-unlock) `efi-unlock`(@df2fe11,MIT,溯 [PZH1gdmu/CMP40HX-Unlock](https://github.com/PZH1gdmu/CMP40HX-Unlock)) |
| 22GiB 布局参数与实测参照 | [KohakuBlueleaf/KohakUnlock](https://github.com/KohakuBlueleaf/KohakUnlock)(GPL-2.0,仅文档参考,未复制代码) |
| 本仓库新增 | 1E04 设备目标移植、22G 几何常量、本机 FWSEC 适配、bootmgfw 链载三连修(见下)、ESP/NVRAM 安装工具、自举式 dump 构建 |

**本移植修复的上游缺陷**(都在 `efi-unlock-tu102/PORT-NOTES.md` 有记录):
1. `u40x_vbios_dump` 双重 BAR0 基址(仅特定主机碰巧可用)
2. 链载自检比较文件大小 vs 内存镜像大小(永不命中→U 盘自循环)
3. RAM(SourceBuffer)加载 bootmgfw:其先 `ExitBootServices` 再因无 BCD 句柄中止 →
   固件被迫整机重 POST → 解锁丢失。已移除,改为 DevicePath 文件加载
4. GPT 全盘枚举得到的设备路径缺 HD 分区节点 → `LoadImage` NOT_FOUND。
   现按分区起始 LBA 反查分区句柄、重构固件认可的完整路径
5. **双卡串行按序解锁支持**: 针对双卡平台（主显示卡 + 副无头计算卡）实现按序隔离预检与原子解锁
6. **热重启寄存器漂移修复**: 取消对非目标寄存器 SS0/SS1 的强等门禁，消除 Windows 热重启后拦截 `0x800000000000001a`
7. **零依赖极速评测与 GSP 助手**: 提供 `bench/quick_fp_bench.py`（免装 PyTorch，1.5 秒即得真实 TFLOPS）与 `tools/set-gsp.ps1` 一键管理 GSP 模式（消灭代码 43）
8. **完整 Windows 部署踩坑实录与启动盘集成**: 详见 [docs/WINDOWS_DEPLOYMENT_NOTES.md](docs/WINDOWS_DEPLOYMENT_NOTES.md)

## 硬件/环境要求

- RTX 2080 Ti **22GB 改装卡**(`10de:1e04`;22G 同源 VBIOS 的卡,其原生 FWSEC 与
  测试卡逐字节一致的概率很高,构建时会校验)
- Windows 10/11 UEFI 启动,GPT,BitLocker 关闭(或先解密)
- BIOS:**Above 4G Decoding=开**、**Secure Boot=关**、**Fast Boot=关**
- 主板:AMD AM4(AGESA)实测;CF8 探测路径通用,Intel 理论可行未测
- 构建:任意 Linux(WSL2 即可)+ `build-essential gnu-efi`
- 注册表:`HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\<你的卡>`
  下 `EnableGpuFirmware=1`(DWORD)——让 Windows 驱动走 GSP-RM 接受 pre-OS 状态

## 快速开始(自举流程)

```bash
# 0) WSL2/Ubuntu 里:
sudo apt install build-essential gnu-efi

# 1) 取得四个 TU102 blob(NVIDIA 固件,自行获取,勿分发/勿提交):
cd tools && bash get-blobs.sh && cd ..

# 2) 无卡也能先构建"转储版"(零固件依赖,只读操作):
cd efi-unlock-tu102 && DUMP_ONLY=1 bash build.sh
# -> TU102DUMP.EFI。用 tools\make-usb.ps1 做成 U 盘,F8 从 U 盘引导一次:
#    屏幕会打印寄存器预检 + 把两个 1MiB PROM 窗口写到 U 盘根目录(只读,无任何写卡操作)

# 3) 提取本机 FWSEC(驱动解析器同款走位,MIT 脚本):
python3 extract_fwsec.py <U盘>\tu102_prom_300000.bin blobs/
# 期望 fwsec_50hx_prod.bin == c6fa28fa...(两张独立卡已验证一致)
mv blobs/fwsec_50hx_prod.bin blobs/fwsec_native.bin

# 4) 构建解锁版(哈希强校验):
bash build.sh   # -> TU102UNLK.EFI
```

### 安装到内置 ESP(推荐,免 U 盘)

```powershell
# 管理员 PowerShell:
# 0) 设置 EnableGpuFirmware=1(见上路径)
# 1) 安装:EFI 入 ESP + 克隆 Windows Boot Manager 设备路径创建 Boot0080 + 全量 NVRAM 备份
tools\install-esp.ps1 -BootNext      # BootNext=下次开机自动跑一次
# 2) 重启 -> 自动解锁并直通 Windows -> tools\..\bench\bench_gemm.py 验证
```

之后:**F8 → "TU102 Unlock"** 随选;**正常开机 = 原厂**(若刚解锁过,完全关机断电一次彻底清态)。

### 使用 U 盘(救援/无 NVRAM 写入意愿)

```powershell
tools\make-usb.ps1 -DriveLetter E -Image ..\efi-unlock-tu102\TU102UNLK.EFI
```

### 验证

```powershell
python bench\bench_gemm.py        # FP16 GEMM 对照 FP32;解锁后约 1.5~1.75×
```

EFI 侧成功标志(日志在引导介质根 `50hx_log.txt`):`*** UNLOCKED (SS0=0x88888888 SS1=0x8) ***`。

## 回退

| 层级 | 操作 |
|---|---|
| 临时回厂 | 正常开机 + 完全关机断电(易失状态清零) |
| 移除 ESP 方案 | `tools\remove-esp.ps1`(删 Boot0080/BootNext + 恢复 BootOrder 备份 + 删 ESP 文件) |
| 完全原厂 | 再删 `EnableGpuFirmware` 注册表值 + BIOS 恢复 Above 4G/Secure Boot/Fast Boot |

## 故障排除(均为本机踩过的坑)

| 症状 | 原因/处置 |
|---|---|
| 选启动项后 Windows 报 `0xc000007b 文件:\EFI\TU102\...EFI` | 该路径被 bootmgfw 而非固件加载(bcdedit 复制条目在某些固件上如此)→ 用本仓库 `install-esp.ps1` 的原生 NVRAM 条目方案 |
| EFI 黑屏数秒后进 BIOS 设置 | 固件未能执行镜像(检查 FAT32/MBR 活动分区/`EFI\Boot\BOOTX64.EFI` 路径),或换 U 盘 |
| `chainload-pre: StartImage returned: ...11` | RAM 加载 bootmgfw 的必然失败(已移除该路径,若见此行说明用了旧版) |
| `file LoadImage: ...0E` 后 `partition dp` 命中即可 | 首选路径缺分区节点属预期,自动兜底 |
| 解锁后进 Windows 但基准没变 | 链载途中发生了重 POST(看日志最后一行)+ 确认 `EnableGpuFirmware=1` |
| `capacity=... != 0x2c9 - refusing` | 不是 22G 布局的卡,守卫拒绝(设计行为) |

## 安全边界

- 解锁为**易失寄存器状态**,不写 VBIOS/熔丝;断电即失
- 已知失败模式:漏洞链执行失败可能挂死 GPU 直到**完全断电冷启动**(全程 3/3 成功,
  但无恢复保证);pre-OS 阶段操作,失败不会损坏系统盘
- 容量寄存器 0x100ce0==0x2c9 身份守卫 + 全部输入哈希校验,防误用于其他卡
- **长时间稳定性未认证**(上游亦未);功耗上限不变,注意散热
- 本方案涉及绕过 NVIDIA 固件保护机制,可能违反 EULA;仅限自有硬件研究
- **绝不**提交/分发 `blobs/*.bin`、`*.EFI`、PROM 转储、NVRAM 备份(`.gitignore` 已排除)

## 许可与致谢

- 本仓库代码:MIT(见 `LICENSE`),含自 cmp50hx-unlock / CMP40HX-Unlock 继承的版权与许可文本
  (`efi-unlock-tu102/LICENSE-40HX-UNLOCK`)
- KohakUnlock(GPL-2.0)与 Turing-unlock(copyleft-next):**仅参考其公开文档中的
  实测常量与方法论,未复制任何代码**,特此致谢;另谢 [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock)
  (Ampere 参考)与 Jon Pry 的 V67 canary 研究
- NVIDIA 固件归 NVIDIA 所有,不入库、不分发

## 仓库结构

```
efi-unlock-tu102/   移植源码 unlock50x_v1.c + build.sh + extract_fwsec.py + PORT-NOTES.md
tools/              get-blobs.sh / install-esp.ps1 / remove-esp.ps1 / make-usb.ps1 / qemu-test.sh
bench/              bench_gemm.py(A/B 基准)
docs/               RESULTS.md(实测数据)
```
