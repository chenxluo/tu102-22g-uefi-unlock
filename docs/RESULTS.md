# 实测结果(Measured results)

一台机器、一张卡、同一脚本(PyTorch 2.1.2+cu118,Windows 11,驱动 621.01)上的完整 A/B。
解锁态经"U 盘引导"与"内置 ESP BootNext"两条路径各验证一次,数值一致。

## 硬件与流程

| 项目 | 值 |
|---|---|
| GPU | RTX 2080 Ti 22 GiB 改装卡(TU102,`10de:1e04`,公版子系统,VBIOS 90.02.33.00.2f) |
| 平台 | AM4(B350)+ Ryzen 7 5700X3D,UEFI,Above 4G 开 / Secure Boot 关 / Fast Boot 关 |
| 原生 FWSEC | 自机 PROM 转储提取,sha256 `c6fa28fa…`(与 KohakUnlock 文档的 Leadtek 卡逐字节一致) |
| 解锁证据 | EFI 日志 `*** UNLOCKED (SS0=0x88888888 SS1=0x8) ***`,寄存器回读;解锁执行 3/3 成功 |
| 交棒 | DevicePath 文件加载 bootmgfw,无重 POST;`EnableGpuFirmware=1` 下驱动接受 pre-OS 状态 |

## 吞吐(bench/bench_gemm.py,中位数)

| 工作负载 | 锁定(冷启动) | 解锁 | 提升 |
|---|---:|---:|---:|
| FP16 GEMM 2048³(FP32 累加) | 37.8 TFLOPS | 64.6 TFLOPS | **+71%** |
| FP16 GEMM 3072³ | 38.3 TFLOPS | 67.1 TFLOPS | **+75%** |
| FP16 GEMM 4096³(持续负载) | 49.9~53.7 TFLOPS | 80.3~81.3 TFLOPS | **+51~62%** |
| FP32 GEMM 2048³(对照) | 12.5 TFLOPS | 12.0 TFLOPS | 持平(噪声内) |

注:短脉冲与持续负载两种跑法都记录过,上表按各自方法学配对比较;4096³ 为
200 次预热 + 100 次计时的持续结果。参考:KohakUnlock 在同型卡(Linux)上
mmapeak 指令级 +85%、cuBLAS 扫频 +53%,与本结果形态一致。

## 数值正确性

FP16 GEMM(512³,标定幅度)对 FP32 参照:最大相对误差 **2.5e-4** —— 标准
FP32 累加精度,无异常。驱动健康:无新增 Xid,22 GiB 显存完整。

## 未验证项(诚实声明)

- 长时间稳定性/耐久(连续烤机数小时级别)未做
- 多卡、其他 22G 卡个体、Intel 平台、11G 卡(会被守卫拒绝)未测
- Windows 大版本更新/驱动大版本升级后的 GSP 接受行为仅验证过 621.01 一版
  (机制上与驱动版本无关,但未逐一验证)
