# TU102 双卡 22GB Windows 解锁与部署踩坑全记录

本文档详细记录了在 Windows 10/11 x64 双路 RTX 2080 Ti 22GB (TU102-300 Non-A) 硬件平台上，推进 UEFI 预启动固件解锁、消除代码 43、排查热重启寄存器漂移、以及对齐原项目（KohakUnlock）真实浮点算力评测标准的完整工程经验与避坑要点。

---

## 一、 硬件配置与 VBIOS 同构化方案

### 1. 原卡初始硬件状态（异构双卡）
在方案实施初期，测试平台所搭载的两张显卡为不同品牌的异构组合：
- **卡 0（Slot 1，PCIe `0000:17:00.0`）**：
  - **原始型号**：技嘉 GeForce RTX 2080 Ti Gaming OC 11G (Gigabyte)
  - **原始 Subsystem ID**：`1458:37c0` (`0x37c01458`)
  - **显示接口**：物理直连主显示器（DP 连接至 AOC U2777B，1080p@60Hz）。
- **卡 1（Slot 4，PCIe `0000:65:00.0`）**：
  - **原始型号**：华硕 GeForce RTX 2080 Ti Dual 11G (ASUS Dual)
  - **原始 Subsystem ID**：`1458:37c4` (`0x37c41458`)
  - **显示接口**：无物理显示输出连接，作为无头（Headless）高性能纯计算卡。

### 2. 物理显存硬改（VRAM Hardware Mod）
- 两张显卡均已在硬件层面进行了显存扩容改装：
  - 拆卸原厂 11 颗 8Gbit (1GB) GDDR6 颗粒；
  - 换焊为 11 颗 16Gbit (2GB) GDDR6 颗粒（单卡物理显存扩展至 22GB / 22,528 MiB）；
  - 调整显卡 PCB 上的显存容量配置电阻（Strap Resistors），使内存控制器（FB）上电识别 22GB 物理空间。

### 3. 方案 B：全量刷入影驰 310W 公版统一固件（VBIOS 同构化）
为了解决异构双卡在 UEFI 预启动阶段的固件差异，两张卡均已全量刷入方案 B 统一固件：
- **固件文件**：`276672.rom` (影驰 Galax 310W 公版统一固件)
- **VBIOS 版本**：`90.02.30.40.50`
- **功耗限制（TDP）**：默认 250W，上限开放调节至 310W
- **统一 Subsystem ID**：统一刷入为 NVIDIA 公版 `10de:1e04` (`0x1e0410de`)
- **固件完整哈希 (ROM SHA256)**：`A84A7EBFA071749AD196D54CEDE36794387C77ED5B4E1978928705D8252384A0`
- **提取 FWSEC 固件块哈希**：`c1dd37237e289ba2a29d943f56878af21f6070681b26128cc2bc030aeb02b00c` (39,968 字节)

#### 为什么必须进行 VBIOS 同构化？
1. **统一 FWSEC 固件块与验签协议**：技嘉与华硕原厂固件中的 FWSEC 长度、偏移和散列签名各不相同。若采用异构固件，UEFI 解锁程序在串行调度时必须切换并维护多套签名；刷入统一公版固件后，两张卡的 PROM、FWSEC 与 Falcon 握手完全同构化。
2. **Subsystem ID 归一化**：将 Subsystem ID 统一为 `10de:1e04` 后，消除了跨品牌混插在 UEFI 阶段的设备匹配分歧，两张卡可共享同一个严格白名单与执行管线。
3. **Windows 驱动加载一致性**：避免了 Windows 系统将两卡识别为不同 OEM 设备导致的驱动分支差异与 P-State 频率调度不一致。

---

## 二、 核心问题与解决方案

### 1. 代码 43（Code 43）的根源与终极解法（方案 B：GSP 微固件模式）

#### 现象
- 在 UEFI 阶段执行固件事务解锁后，顺利链式引导进入 Windows；
- 设备管理器中双卡均报**代码 43（Code 43）**；
- 尝试在设备管理器中“禁用设备再启用”，会直接导致屏幕黑屏，随后触发 Windows 内核崩溃（BSOD）与系统自动重启。

#### 机理分析
- **传统 CPU RM 冲突**：Windows 下 NVIDIA 驱动（`nvlddmkm.sys`）默认在 CPU 侧运行 Resource Manager（CPU RM）。当检测到 SEC2/Falcon 相关控制寄存器被外部固件篡改后，安全校验失败，主动卸载驱动并抛出代码 43；禁用重开则会引发微控制器死锁。
- **GSP-RM 机制**：从 R510 系列驱动开始，NVIDIA 为 Turing 及之后架构引入了 GSP 微固件运行模式（GSP-RM），由 GPU 板载的 GSP Falcon 芯片独立托管底层资源管理。GSP 模式对 WPR2/FRTS 显存保护区的几何结构天然兼容，完全绕过了 CPU RM 的严苛本地静态校验。

#### 解决方案
在 Windows 注册表中，为两张显卡的驱动配置项开启 `EnableGpuFirmware`：
```cmd
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\0006" /v EnableGpuFirmware /t REG_DWORD /d 1 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\0007" /v EnableGpuFirmware /t REG_DWORD /d 1 /f
```
- **效果**：系统重启后，驱动以 `GSP Firmware Version: 615.78.05` 模式加载，**彻底根除代码 43**！双卡在设备管理器中显示正常（Status: OK），物理显示器正常点亮输出（60Hz），`nvidia-smi` 正确识别双卡各 22528 MiB。
- **灾备保护**：在桌面提供 `一键恢复默认显卡驱动.bat`，若遇异常可在安全模式下双击一键清除注册表项还原。

---

### 2. 显存 22GB 与 Tensor Core 算力的本质区别（评测避坑指南）

#### 易混淆误区
在实际测试中发现：**系统重启多次未跑 EFI 解锁，Windows 下仍然显示 22GB 且无代码 43，同时 FP32 跑出了 15.8 TFLOPS，被误以为已经“解锁”**。

#### 事实真相与科学边界
1. **22GB 显存容量**：
   - 显存大小完全由**物理硬改颗粒**和**刷入 EEPROM 的 22GB VBIOS 固件**决定；
   - 即使**完全不运行 EFI 解锁程序**，只要显卡刷了 `276672.rom` 且开启了 GSP，进入 Windows 也天然显示 22GB。
2. **基础 FP32 单精度浮点**：
   - RTX 2080 Ti 拥有 4352 个 CUDA 核心，在未解锁状态下，其 FP32 FMA 算力天然就是 **15.5 ~ 16.0 TFLOPS**；
   - 测量纯 FP32 算力**无法**用来判定是否成功解锁！
3. **原项目（KohakUnlock）真正解锁的核心对象**：
   - NVIDIA 在出厂微固件中人为将 2080 Ti 的 **Tensor Core（张量核心，HMMA FP16输入+FP32累加）限制为半速**；
   - **出厂未解锁（当前未过 EFI 状态）**：4096 GEMM 实测张量算力仅为 **~40 ~ 42 TFLOPS**；
   - **EFI 固件解锁后**：SEC2 Falcon 固件解密，解除半速限速，算力飙升至 **88 ~ 111 TFLOPS（性能翻倍！）**。

#### 标准验证方法
使用桌面部署的 `双卡浮点算力实测.bat`（调用 `tools/quick_fp_bench.py`），直接调用 cuBLAS 运行 4096 FP16→FP32 GEMM：
- 若显示 `~41 TFLOPS`：表示开机未过 EFI，处于出厂半速锁定状态；
- 若显示 `>85 TFLOPS`：表示已由 EFI 解除限速，张量算力已满血释放！

---

### 3. 热重启寄存器漂移与过度拦截的消除

#### 现象
开机启动 EFI 解锁程序时，屏幕一闪而过并报错提示：`CARD1_BOUND_PREFLIGHT 0x800000000000001a`，停在 `Press any key to continue...`，按键后跳过解锁直接进了 Windows。

#### 机理分析
- **寄存器含义**：`0x00409664`（SS0）与 `0x0040966c`（SS1）是 GPU 图形核心（GR Engine）的 SM 指令发射率（Issue Rate Modifier）与能耗调度选择器。
- **冷启动 vs 热重启**：
  - 冷启动（完全断电开机）：GPU 核心电平彻底复位，寄存器保持 `0/0` 或冷态初值；
  - 热重启（Windows 下点击重启）：PCIe 总线仅软复位，GPU 供电不切断。Windows 驱动和 GSP 加载运行期间写入的动态调度状态（例如本次读出的 `SS1 = 5`）会被保留到下一次 UEFI 启动阶段。
  - 主卡（有输出，处于 0/6 或 0/7）与副卡（无头计算，处于 0x02040000/4 或 5）状态各不相同。
- **过度约束**：原版 KohakUnlock 针对单卡冷启动硬编码了强等断言（`actual == expected`）。但整个解锁事务中**从来不修改这两个寄存器**，它们不是解锁目标，也不是安全边界。强行限制其数值只会造成热重启频繁拦截。

#### 解决方案
在 `uefi/tu102_dual_unlock.c` 中解除 SS0/SS1 的强等门禁：
- **核心安全锁保留**：严格比对芯片架构 ID（TU102 BOOT0 `0x162000a1`）、显存容量条带（`0x00100ce0`）、Falcon 停机状态（CPUCTL `0x10`）、BAR0 映射及 VBIOS/FWSEC 校验。
- **动态状态降级**：SS0/SS1 仅作为普通状态读取并记入日志，不再触发预检失败，彻底杜绝了热重启拦截。

---

### 4. Windows 脚本与编码规范细节

- **批处理编码**：Windows CMD 在中文系统下默认使用 CP936（GBK）代码页。包含中文注释、提示语或路径的 `.bat` 文件必须使用 **ANSI/GBK 格式** 保存，避免发生中文字符解析异常（如 `'o.' 不是内部或外部命令`）。
- **零依赖测试**：为避免测试环境依赖数 GB 的 PyTorch/CUDA Toolkit，检测工具直接通过 Python `ctypes` 调用 Windows 系统驱动自带的 `nvcuda.dll` 和 `cublas64_11.dll`，1.5 秒极速给出精准硬件结果。

---

## 三、 集成 EFI 到启动介质的操作指南（U盘 / ESP 分区常驻 / NVRAM BootNext）

本方案解锁状态属于**寄存器易失状态**（断电或冷开机后自动归零回厂）。若要在日常使用中快速或无感加载，可根据稳定性需求选择以下集成方案：

### 1. 方案一：外置 U 盘 / Ventoy 启动盘（隔离度高，测试与救援首选）

#### 方式 A：Ventoy U 盘直读（当前实测方案）
- **操作步骤**：
  1. 将编译好的 `TU102-DUAL-UNLOCK-V4.EFI` 复制至已安装 Ventoy 的 U 盘中（如 `D:\TU102-V4\TU102-DUAL-UNLOCK-V4.EFI`）；
  2. 重启电脑，按主板引导快捷键（华硕/技嘉 F8/F12，联想 F12）选择从 Ventoy U 盘启动；
  3. 在 Ventoy 菜单主界面按 `F6`（或进入文件浏览模式），找到并选中该 `.efi` 文件回车执行；
  4. 屏幕提示 `*** DUAL UNLOCK COMPLETE ***` 后回车，程序将自动链式引导主板或硬盘上的 `\EFI\Microsoft\Boot\bootmgfw.efi`，无缝进入 Windows。

#### 方式 B：纯 FAT32 单 EFI 启动盘
- **操作步骤**：
  1. 准备一个普通 U 盘，格式化为 `FAT32` 文件系统；
  2. 在根目录创建目录结构：`\EFI\Boot\`；
  3. 将 `TU102-DUAL-UNLOCK-V4.EFI` 重命名并放置为 `\EFI\Boot\BOOTX64.EFI`；
  4. 开机按 F8/F12 选 U 盘，主板固件会自动加载默认引导程序直接执行解锁。

---

### 2. 方案二：集成至内置硬盘 ESP 分区（免插 U 盘，日常无盘化使用）

将 EFI 镜像部署至系统启动固态硬盘的 ESP 分区（EFI System Partition），并通过 NVRAM 引导项实现开机自动解锁。

#### 步骤一：挂载系统 ESP 分区并部署 EFI
以**管理员身份**打开命令提示符（CMD）或 PowerShell：
```cmd
:: 1. 将系统内置 ESP 隐藏分区挂载为盘符 S:
mountvol S: /s

:: 2. 创建专用目录
mkdir S:\EFI\TU102

:: 3. 将解锁 EFI 复制到 ESP 分区
copy TU102-DUAL-UNLOCK-V4.EFI S:\EFI\TU102\TU102UNLK.EFI

:: 4. (可选) 操作完成后卸载挂载点以保证安全
mountvol S: /d
```

#### 步骤二：配置 UEFI NVRAM 启动项（两种策略）

##### 策略 A：BootNext 单次按需自启（最安全，零死循环风险）
- **适用场景**：日常办公使用原版开机；需要跑深度学习或高性能推理时，在 Windows 下一键设定“下次重启自动解锁”。
- **原理**：修改 UEFI 规范定义的全局变量 `BootNext`。
- **操作方法**：
  以管理员权限运行本仓库提供的 PowerShell 脚本：
  ```powershell
  powershell -ExecutionPolicy Bypass -File tools\install-esp.ps1 -BootNext
  ```
  该命令会自动：
  1. 自动备份当前 NVRAM 启动顺序（`BootOrder`）；
  2. 基于 Windows Boot Manager 所在物理分区克隆生成 `Boot0080` 项（指向 `\EFI\TU102\TU102UNLK.EFI`）；
  3. 将 `BootNext` 设置为 `Boot0080`；
  4. 重启计算机后，主板仅在下一次启动优先加载解锁 EFI，直通进入 Windows 后 `BootNext` 自动失效；若后续冷启动，依然恢复为默认原厂状态。

##### 策略 B：常驻首位自动引导（完全无感自启）
- **适用场景**：长期从事模型微调或高强度计算，希望每次开机均自动保持算力翻倍。
- **操作方法**：
  以管理员权限运行：
  ```powershell
  powershell -ExecutionPolicy Bypass -File tools\install-esp.ps1
  ```
  该命令会将 `Boot0080` 插入至 `BootOrder` 的最首位（优先级高于 Windows Boot Manager）。
- **执行流**：
  `开机 -> 主板加载 Boot0080 (TU102UNLK.EFI) -> 串行解锁双卡 -> 链式调用 bootmgfw.efi -> 进入 Windows`。

#### 步骤三：一键卸载与安全回退
如果因驱动变动、BIOS 更新或不需要解锁常驻时，可随时执行快速卸载：
```powershell
powershell -ExecutionPolicy Bypass -File tools\remove-esp.ps1
```
- **自动恢复**：自动删除 NVRAM `Boot0080` 和 `BootNext`，还原初始 `BootOrder` 备份，并清理 ESP 分区内的 `\EFI\TU102` 目录。
- **物理容灾**：即便 NVRAM 项异常，长按电源键强制关机断电，寄存器即刻恢复原厂，按主板 F8 仍可强制选择原版 "Windows Boot Manager" 正常开机。

---

## 四、 维护工具速查

| 工具文件 | 路径 / 形式 | 作用 |
|---|---|---|
| **双卡浮点算力实测.bat** | 桌面快捷脚本 | 对标 KohakUnlock 原项目标准，实测双卡 Tensor Core 4096 GEMM TFLOPS（判定是否解除半速限制） |
| **一键开启GSP模式.bat** | 桌面快捷脚本 | 注入 `EnableGpuFirmware=1`，彻底消除 Windows 代码 43 |
| **一键恢复默认显卡驱动.bat** | 桌面快捷脚本 | 清除 GSP 注册表，恢复原生驱动传统模式（支持安全模式运行） |
| **TU102-DUAL-UNLOCK-V4.EFI** | `D:\TU102-V4\` | 部署在 U 盘上的最新版预启动双卡算力解锁固件 |
