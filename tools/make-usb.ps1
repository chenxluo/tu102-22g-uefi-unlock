# make-usb.ps1 — 制作解锁/转储 U 盘(救援或初始 dump 用)
# 用法: 管理员 PowerShell 运行: .\make-usb.ps1 -DriveLetter E -Image .\..\efi-unlock-tu102\TU102DUMP.EFI
# 警告:目标 U 盘将被完全格式化!
param([Parameter(Mandatory=$true)][string]$DriveLetter,
      [Parameter(Mandatory=$true)][string]$Image)

$ErrorActionPreference = 'Stop'
$p = Get-Partition -DriveLetter $DriveLetter[0] -ErrorAction Stop
$d = Get-Disk -Number $p.DiskNumber
if ($d.BusType -ne 'USB') { throw "目标 $($DriveLetter): 不是 USB 总线($($d.BusType)) — 拒绝格式化" }
Write-Host "将完全清除 USB 盘: Disk $($d.Number) $($d.FriendlyName) ($([math]::Round($d.Size/1GB,2))GB)"
$confirm = Read-Host '输入 YES 确认'
if ($confirm -ne 'YES') { throw '用户取消' }

Clear-Disk -Number $d.Number -RemoveData -RemoveOEM -Confirm:$false
Initialize-Disk -Number $d.Number -PartitionStyle MBR
$part = New-Partition -DiskNumber $d.Number -Size 1GB -IsActive -AssignDriveLetter
Format-Volume -Partition $part -FileSystem FAT32 -NewFileSystemLabel 'TU102' | Out-Null
$u = "$($part.DriveLetter):"
New-Item -ItemType Directory -Force "$u\EFI\Boot" | Out-Null
Copy-Item $Image "$u\EFI\Boot\BOOTX64.EFI" -Force
$zero = New-Object byte[] (1MB)
[IO.File]::WriteAllBytes("$u\tu102p30.bin", $zero)
[IO.File]::WriteAllBytes("$u\tu102p11.bin", $zero)
Write-Host "完成: $u\EFI\Boot\BOOTX64.EFI (sha256 $((Get-FileHash "$u\EFI\Boot\BOOTX64.EFI").Hash.Substring(0,16))...)"
Write-Host '开机 F8 选择 UEFI: <该U盘> 即从 U 盘引导。'
