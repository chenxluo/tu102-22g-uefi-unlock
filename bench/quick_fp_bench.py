# -*- coding: utf-8 -*-
"""
TU102 Dual/Single GPU Tensor Core (FP16->FP32 HMMA) Throughput Benchmark
TU102 双卡/单卡 Tensor Core (FP16->FP32 HMMA) 浮点算力极速实测工具

Benchmarking standard aligned with KohakUnlock:
- Locked (stock half-rate throttle / 未解锁): ~40 - 55 TFLOPS
- Unlocked (pre-OS EFI unthrottled / 已解锁): ~88 - 110 TFLOPS (2x speedup)
"""
import os
import sys
import time
import ctypes
from pathlib import Path

def find_cuda_libraries():
    script_dir = Path(__file__).resolve().parent
    cuda_lib = None
    cublas_lib = None

    if sys.platform == "win32":
        # Search candidate paths for Windows
        search_dirs = [
            script_dir,
            script_dir.parent / "tools",
            Path(os.environ.get("CUDA_PATH", "")) / "bin",
            Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
        ]
        for d in search_dirs:
            if d.exists():
                try:
                    os.add_dll_directory(str(d))
                except Exception:
                    pass

        # Load CUDA driver
        try:
            cuda_lib = ctypes.WinDLL("nvcuda.dll")
        except Exception:
            pass

        # Load cuBLAS (try 11, 12, or local)
        cublas_candidates = [
            "cublas64_11.dll",
            "cublas64_12.dll",
            str(script_dir / "cublas64_11.dll"),
            str(script_dir.parent / "tools" / "cublas64_11.dll")
        ]
        for c in cublas_candidates:
            try:
                cublas_lib = ctypes.WinDLL(c)
                if cublas_lib:
                    break
            except Exception:
                continue
    else:
        # Linux
        for c_name in ("libcuda.so", "libcuda.so.1"):
            try:
                cuda_lib = ctypes.CDLL(c_name)
                break
            except Exception:
                pass
        for cb_name in ("libcublas.so", "libcublas.so.11", "libcublas.so.12"):
            try:
                cublas_lib = ctypes.CDLL(cb_name)
                break
            except Exception:
                pass

    return cuda_lib, cublas_lib

def main():
    print("=" * 68)
    print("      TU102 Tensor Core GEMM Unlock Benchmark / 算力解锁快速评测")
    print("=" * 68)
    print()

    cuda, cublas = find_cuda_libraries()
    if not cuda or not cublas:
        print("[ERROR / 错误] Failed to load CUDA driver (nvcuda) or cuBLAS library.")
        print("               无法加载 CUDA 驱动或 cuBLAS 动态链接库。")
        print("               Please ensure NVIDIA driver is installed, or put cublas64_11.dll in tools/.")
        if sys.stdin.isatty():
            input("\nPress Enter to exit / 按回车退出...")
        return 1

    if cuda.cuInit(0) != 0:
        print("[ERROR / 错误] cuInit(0) failed / CUDA 初始化失败。")
        if sys.stdin.isatty():
            input("\nPress Enter to exit / 按回车退出...")
        return 1

    count = ctypes.c_int()
    cuda.cuDeviceGetCount(ctypes.byref(count))
    if count.value < 1:
        print("[ERROR / 错误] No NVIDIA GPU detected / 未检测到可用的 NVIDIA 显卡。")
        if sys.stdin.isatty():
            input("\nPress Enter to exit / 按回车退出...")
        return 1

    print(f"Detected {count.value} GPU(s). Running 4096 GEMM Tensor Core (FP16->FP32) test...\n")

    N = 4096
    elem_count = N * N
    bytes_f16 = elem_count * 2
    bytes_f32 = elem_count * 4
    flops = 2 * (N ** 3)

    CUBLAS_COMPUTE_32F = 68
    CUDA_R_16F = 2
    CUDA_R_32F = 0
    alpha = ctypes.c_float(1.0)
    beta = ctypes.c_float(0.0)

    any_unlocked = False
    all_unlocked = True

    for i in range(count.value):
        dev = ctypes.c_int()
        cuda.cuDeviceGet(ctypes.byref(dev), i)
        name_buf = ctypes.create_string_buffer(256)
        cuda.cuDeviceGetName(name_buf, 256, dev)
        dev_name = name_buf.value.decode("utf-8", errors="replace")

        ctx = ctypes.c_void_p()
        if cuda.cuCtxCreate_v2(ctypes.byref(ctx), 0, dev) != 0:
            print(f"  [GPU {i}] cuCtxCreate failed")
            all_unlocked = False
            continue

        handle = ctypes.c_void_p()
        if cublas.cublasCreate_v2(ctypes.byref(handle)) != 0:
            print(f"  [GPU {i}] cublasCreate failed")
            cuda.cuCtxDestroy_v2(ctx)
            all_unlocked = False
            continue

        cublas.cublasSetMathMode(handle, 1)  # CUBLAS_TENSOR_OP_MATH

        d_A = ctypes.c_uint64()
        d_B = ctypes.c_uint64()
        d_C = ctypes.c_uint64()
        cuda.cuMemAlloc_v2(ctypes.byref(d_A), bytes_f16)
        cuda.cuMemAlloc_v2(ctypes.byref(d_B), bytes_f16)
        cuda.cuMemAlloc_v2(ctypes.byref(d_C), bytes_f32)

        # Warmup
        for _ in range(5):
            cublas.cublasGemmEx(
                handle, 0, 0, N, N, N,
                ctypes.byref(alpha),
                d_A, CUDA_R_16F, N,
                d_B, CUDA_R_16F, N,
                ctypes.byref(beta),
                d_C, CUDA_R_32F, N,
                CUBLAS_COMPUTE_32F, -1
            )
        cuda.cuCtxSynchronize()

        # Timed benchmark: 15 runs
        runs = 15
        t0 = time.time()
        for _ in range(runs):
            cublas.cublasGemmEx(
                handle, 0, 0, N, N, N,
                ctypes.byref(alpha),
                d_A, CUDA_R_16F, N,
                d_B, CUDA_R_16F, N,
                ctypes.byref(beta),
                d_C, CUDA_R_32F, N,
                CUBLAS_COMPUTE_32F, -1
            )
        cuda.cuCtxSynchronize()
        t1 = time.time()
        avg_s = (t1 - t0) / runs
        tflops = (flops / avg_s) / 1e12

        # Turing 2080 Ti: Stock half-rate limit is ~40-55 TFLOPS. Unlocked is >85 TFLOPS.
        is_unlocked = (tflops >= 75.0)
        if is_unlocked:
            any_unlocked = True
            status_tag = "[UNLOCKED / 已解锁 (解除限速)]"
        else:
            all_unlocked = False
            status_tag = "[LOCKED / 未解锁 (出厂半速限速)]"

        print(f"  [GPU {i}] {dev_name}:")
        print(f"    - 4096 GEMM Latency: {avg_s*1000:6.2f} ms")
        print(f"    - Tensor Throughput: {tflops:6.2f} TFLOPS  -->  {status_tag}")

        cuda.cuMemFree_v2(d_A)
        cuda.cuMemFree_v2(d_B)
        cuda.cuMemFree_v2(d_C)
        cublas.cublasDestroy_v2(handle)
        cuda.cuCtxDestroy_v2(ctx)

    print()
    print("-" * 68)
    if all_unlocked:
        print("  RESULT: *** ALL GPU(s) UNLOCKED (>85 TFLOPS) / 全部显卡算力已解锁 ***")
    elif any_unlocked:
        print("  RESULT: Partial GPU(s) unlocked / 部分显卡已解锁。")
    else:
        print("  RESULT: Cards in stock half-rate throttle / 显卡处于出厂半速限速状态 (~40 TFLOPS)。")
        print("          Boot via TU102 unlock EFI to remove the issue-rate limiter.")
    print("-" * 68)
    print()
    if sys.stdin.isatty():
        input("Press Enter to exit / 按回车退出...")
    return 0

if __name__ == "__main__":
    sys.exit(main() or 0)
