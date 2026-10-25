# install-esp.ps1 — 把 TU102UNLK.EFI 装进内置 ESP 并创建固件启动项(无 U 盘方案)
#
# 前提:管理员 PowerShell、Secure Boot 关闭、已构建 efi-unlock-tu102\TU102UNLK.EFI
# 行为:
#   1. 复制 EFI 到 ESP \EFI\TU102\
#   2. 全量备份 Boot####/BootOrder 原始字节到 <repo>\local\nvram-backup\
#   3. 克隆 Windows Boot Manager 启动项的设备路径(硬件+分区节点),仅替换文件路径,
#      写入新固件变量 Boot0080 "TU102 Unlock"(ACTIVE)
#   4. BootOrder 追加到末尾(Windows 仍默认;新项出现在 F8 菜单)
#   5. 可选:-BootNext 同时设置下次开机自动执行一次
# 卸载:remove-esp.ps1
param([switch]$BootNext)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$out = Join-Path $RepoRoot 'local\install-esp-out.txt'
New-Item -ItemType Directory -Force (Join-Path $RepoRoot 'local') | Out-Null
function Log($m) { $m | Out-File -Append -Encoding utf8 $out }
"=== ESP install $(Get-Date -Format T) ===" | Out-File -Encoding utf8 $out

# ---- 0. ESP 准备 ----
mountvol S: /S
if (-not (Test-Path 'S:\EFI\Microsoft\Boot\bootmgfw.efi')) { throw 'ESP mount failed / bootmgfw not found' }
$efi = Join-Path $RepoRoot 'efi-unlock-tu102\TU102UNLK.EFI'
if (-not (Test-Path $efi)) { throw "未找到 $efi(先构建)" }
New-Item -ItemType Directory -Force S:\EFI\TU102 | Out-Null
Copy-Item $efi S:\EFI\TU102\TU102UNLK.EFI -Force
Log("ESP EFI sha256 = " + (Get-FileHash S:\EFI\TU102\TU102UNLK.EFI -Algorithm SHA256).Hash)

# ---- 1. P/Invoke + 特权 ----
$sig = @'
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
public static extern uint GetFirmwareEnvironmentVariableExW(string lpName, string lpGuid, byte[] pBuffer, uint nSize, out uint pdwAttributes);
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
public static extern bool SetFirmwareEnvironmentVariableExW(string lpName, string lpGuid, byte[] pBuffer, uint nSize, uint dwAttributes);
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
$null = Add-Type -MemberDefinition $sig -Name K32C -Namespace Win32 -PassThru
$K = [Win32.K32C]
if (-not $K::EnablePriv()) { throw 'EnablePriv failed' }
Log('SeSystemEnvironmentPrivilege enabled')
$guid = '{8be4df61-93ca-11d2-aa0d-00e098032b8c}'

function Get-BootVar([string]$name) {
    $buf = New-Object byte[] 8192
    $attr = [uint32]0
    $n = $K::GetFirmwareEnvironmentVariableExW($name, $guid, $buf, $buf.Length, [ref]$attr)
    if ($n -gt 0) { $b = New-Object byte[] $n; [Array]::Copy($buf, $b, $n); return @{ Data = $b; Attr = $attr } }
    return $null
}
function Set-BootVar([string]$name, [byte[]]$data, [uint32]$attr) {
    if (-not $K::SetFirmwareEnvironmentVariableExW($name, $guid, $data, $data.Length, $attr)) {
        throw "SetVariable $name failed: err $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
}

# ---- 2. 备份 ----
$bak = Join-Path $RepoRoot 'local\nvram-backup'
New-Item -ItemType Directory -Force $bak | Out-Null
$order = Get-BootVar 'BootOrder'
if (-not $order) { throw 'cannot read BootOrder' }
[IO.File]::WriteAllBytes("$bak\BootOrder.bin", $order.Data)
Log("BootOrder backed up ($($order.Data.Length/2) entries)")
foreach ($i in 0..0x40) {
    $n2 = 'Boot{0:X4}' -f $i
    $v = Get-BootVar $n2
    if ($v) { [IO.File]::WriteAllBytes("$bak\$n2.bin", $v.Data) }
}

# ---- 3. 找到 bootmgfw 的 Boot#### 并克隆设备路径 ----
function Parse-Option([byte[]]$b) {
    $fplLen = [BitConverter]::ToUInt16($b, 4)
    $d = 6
    while ($d + 1 -lt $b.Length -and -not ($b[$d] -eq 0 -and $b[$d+1] -eq 0)) { $d += 2 }
    $desc = [Text.Encoding]::Unicode.GetString($b, 6, $d - 6)
    $list = New-Object byte[] $fplLen
    [Array]::Copy($b, $d + 2, $list, 0, $fplLen)
    $off = 0; $lastFileOff = -1; $fp = ''
    while ($off + 4 -le $list.Length) {
        $t = $list[$off]; $s = $list[$off+1]; $len = [BitConverter]::ToUInt16($list, $off+2)
        if ($len -lt 4 -or $off + $len -gt $list.Length) { break }
        if ($t -eq 4 -and $s -eq 4) { $lastFileOff = $off; $fp = [Text.Encoding]::Unicode.GetString($list, $off+4, $len-6) }
        if ($t -eq 0x7f) { break }
        $off += $len
    }
    return @{ Desc=$desc; List=$list; LastFileOff=$lastFileOff; FilePath=$fp }
}
$srcParsed = $null
foreach ($i in 0..0x40) {
    $v = Get-BootVar ('Boot{0:X4}' -f $i)
    if (-not $v) { continue }
    $p = Parse-Option $v.Data
    if ($p.FilePath -match 'BOOTMGFW\.EFI') { $srcParsed = $p; Log("cloning device path from: $($p.Desc)"); break }
}
if (-not $srcParsed) { throw 'no bootmgfw Boot#### found' }

# ---- 4. 构造新 load option ----
$prefixLen = $srcParsed.LastFileOff
$prefix = New-Object byte[] $prefixLen
[Array]::Copy($srcParsed.List, 0, $prefix, 0, $prefixLen)
$newPath = [Text.Encoding]::Unicode.GetBytes('\EFI\TU102\TU102UNLK.EFI')
$fpLen = 4 + $newPath.Length + 2
$newList = New-Object byte[] ($prefixLen + $fpLen + 4)
[Array]::Copy($prefix, 0, $newList, 0, $prefixLen)
$newList[$prefixLen] = 4; $newList[$prefixLen+1] = 4
[BitConverter]::GetBytes([uint16]$fpLen).CopyTo($newList, $prefixLen+2)
$newPath.CopyTo($newList, $prefixLen+4)
$e = $prefixLen + $fpLen
$newList[$e] = 0x7f; $newList[$e+1] = 0xff
[BitConverter]::GetBytes([uint16]4).CopyTo($newList, $e+2)
$desc = [Text.Encoding]::Unicode.GetBytes('TU102 Unlock')
$opt = New-Object byte[] (4 + 2 + $desc.Length + 2 + $newList.Length)
[BitConverter]::GetBytes([uint32]1).CopyTo($opt, 0)
[BitConverter]::GetBytes([uint16]$newList.Length).CopyTo($opt, 4)
$desc.CopyTo($opt, 6)
$newList.CopyTo($opt, 6 + $desc.Length + 2)

# ---- 5. 写 Boot0080 + BootOrder(+BootNext)----
if (Get-BootVar 'Boot0080') { Log('Boot0080 exists — overwriting') }
Set-BootVar 'Boot0080' $opt 7
Log('Boot0080 "TU102 Unlock" written')
$ordData = New-Object byte[] ($order.Data.Length + 2)
[Array]::Copy($order.Data, $ordData, $order.Data.Length)
if (-not ((0..($order.Data.Length/2-1) | Where-Object { [BitConverter]::ToUInt16($order.Data, $_*2) -eq 0x0080 }))) {
    [BitConverter]::GetBytes([uint16]0x0080).CopyTo($ordData, $order.Data.Length)
    Set-BootVar 'BootOrder' $ordData $order.Attr
    Log('BootOrder: appended 0x0080 at END (Windows stays default)')
}
if ($BootNext) {
    Set-BootVar 'BootNext' ([BitConverter]::GetBytes([uint16]0x0080)) 7
    Log('BootNext=0x0080 (one-shot on next boot)')
}
Log('DONE. F8 -> "TU102 Unlock" to boot unlocked.')
