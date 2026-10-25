# remove-esp.ps1 — 卸载内置 ESP 解锁(还原)
# 需要:管理员 PowerShell;local\nvram-backup\ 里要有 install-esp.ps1 的备份
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$bak = Join-Path $RepoRoot 'local\nvram-backup'
if (-not (Test-Path "$bak\BootOrder.bin")) { throw "未找到备份 $bak\BootOrder.bin" }

$sig = @'
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
public static extern bool SetFirmwareEnvironmentVariableExW(string lpName, string lpGuid, byte[] pBuffer, uint nSize, uint dwAttributes);
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
public static extern uint GetFirmwareEnvironmentVariableExW(string lpName, string lpGuid, byte[] pBuffer, uint nSize, out uint pdwAttributes);
[DllImport("advapi32.dll", SetLastError=true)]
public static extern bool OpenProcessToken(IntPtr h, uint access, out IntPtr tok);
[DllImport("advapi32.dll", SetLastError=true, CharSet=CharSet.Ansi)]
public static extern bool LookupPrivilegeValueA(string sys, string name, out long luid);
[DllImport("advapi32.dll", SetLastError=true)]
public static extern bool AdjustTokenPrivileges(IntPtr tok, bool disableAll, ref TP newst, int len, IntPtr prev, IntPtr ret);
[StructLayout(LayoutKind.Sequential, Pack=4)]
public struct TP { public int Count; public long Luid; public int Attr; }
[DllImport("kernel32.dll")] public static extern IntPtr GetCurrentProcess();
public static bool EnablePriv() {
    IntPtr t; if (!OpenProcessToken(GetCurrentProcess(), 0x28, out t)) return false;
    long luid; if (!LookupPrivilegeValueA(null, "SeSystemEnvironmentPrivilege", out luid)) return false;
    TP tp = new TP(); tp.Count = 1; tp.Luid = luid; tp.Attr = 2;
    AdjustTokenPrivileges(t, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
    return true;
}
'@
$null = Add-Type -MemberDefinition $sig -Name K32R -Namespace Win32 -PassThru
$K = [Win32.K32R]
if (-not $K::EnablePriv()) { throw 'EnablePriv failed' }
$guid = '{8be4df61-93ca-11d2-aa0d-00e098032b8c}'

function Del-BootVar([string]$name) {
    # 以 0 长度 + 原属性删除
    $buf = New-Object byte[] 8; $attr = [uint32]0
    $n = $K::GetFirmwareEnvironmentVariableExW($name, $guid, $buf, 8, [ref]$attr)
    $empty = New-Object byte[] 0
    $null = $K::SetFirmwareEnvironmentVariableExW($name, $guid, $empty, 0, 7)
    Write-Host "deleted $name"
}

Del-BootVar 'Boot0080'
Del-BootVar 'BootNext'
# 恢复 BootOrder 备份
$ord = [IO.File]::ReadAllBytes("$bak\BootOrder.bin")
$null = $K::SetFirmwareEnvironmentVariableExW('BootOrder', $guid, $ord, $ord.Length, 7)
Write-Host 'BootOrder restored from backup'

# 删除 ESP 文件
mountvol S: /S
Remove-Item S:\EFI\TU102 -Recurse -Force -ErrorAction SilentlyContinue
Write-Host 'ESP \EFI\TU102 removed'
Write-Host '完成。建议再完全关机断电一次回到最干净状态。'
Write-Host '(可选)彻底原厂化:删除注册表 EnableGpuFirmware 并恢复 BIOS:Above 4G / Secure Boot / Fast Boot'
