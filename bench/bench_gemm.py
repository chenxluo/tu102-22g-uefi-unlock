# A/B benchmark for Turing tensor unlock: FP16 GEMM (FP32 accumulate = unlock target)
# and FP32 GEMM (control, expected unchanged).
# Methodology follows KohakUnlock RESULTS.md conventions loosely:
# fixed shape list, warmup + timed iterations, median of windows, TFLOPS.
import json, statistics, sys, time
import torch

torch.backends.cuda.matmul.allow_tf32 = False  # Turing has no TF32 path anyway; keep FP32 pure
torch.backends.cudnn.benchmark = False

dev = torch.device("cuda:0")
props = torch.cuda.get_device_properties(0)
print(f"GPU: {props.name}  SMs={props.multi_processor_count}  VRAM={props.total_memory/2**30:.1f} GiB")
print(f"torch {torch.__version__}  cuda {torch.version.cuda}")
uuid = getattr(torch.cuda.get_device_properties(0), "uuid", None)
print(f"UUID: {uuid}")

def bench(mode, n, warmup=10, iters=30):
    a = torch.randn((n, n), device=dev, dtype=mode)
    b = torch.randn((n, n), device=dev, dtype=mode)
    for _ in range(warmup):
        c = a @ b
    torch.cuda.synchronize()
    times = []
    for _ in range(iters):
        s, e = torch.cuda.Event(True), torch.cuda.Event(True)
        s.record()
        c = a @ b
        e.record()
        torch.cuda.synchronize()
        times.append(s.elapsed_time(e))  # ms
    med = statistics.median(times)
    tflops = 2 * n**3 / (med / 1e3) / 1e12
    return med, tflops

results = {"meta": {"gpu": props.name, "sms": props.multi_processor_count,
                    "vram_gib": round(props.total_memory / 2**30, 1),
                    "torch": torch.__version__, "cuda": torch.version.cuda,
                    "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
                    "power_state_note": "record nvidia-smi power/clocks separately"},
           "fp16_gemm_fp32acc": {}, "fp32_gemm_control": {}}

for n in (2048, 3072, 4096):
    ms, tf = bench(torch.float16, n)
    results["fp16_gemm_fp32acc"][str(n)] = {"median_ms": round(ms, 3), "tflops": round(tf, 2)}
    print(f"FP16  {n}^3: {ms:8.2f} ms  {tf:7.2f} TFLOPS")

for n in (2048,):
    ms, tf = bench(torch.float32, n)
    results["fp32_gemm_control"][str(n)] = {"median_ms": round(ms, 3), "tflops": round(tf, 2)}
    print(f"FP32  {n}^3: {ms:8.2f} ms  {tf:7.2f} TFLOPS (control)")

out = sys.argv[1] if len(sys.argv) > 1 else "results.json"
with open(out, "w", encoding="utf-8") as f:
    json.dump(results, f, indent=2, ensure_ascii=False)
print(f"saved -> {out}")
