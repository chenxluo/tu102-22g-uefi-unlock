# set-gsp.ps1 — Configure NVIDIA GSP Mode (EnableGpuFirmware) on Windows
#
# Usage (Run as Administrator in PowerShell):
#   .\tools\set-gsp.ps1           # Enable GSP mode on all detected NVIDIA GPUs
#   .\tools\set-gsp.ps1 -Disable  # Remove EnableGpuFirmware (revert to default driver)
#   .\tools\set-gsp.ps1 -Status   # Show current GSP mode status
#
param(
    [switch]$Disable,
    [switch]$Status
)

$ErrorActionPreference = 'Stop'
$classPath = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Error "Please run this script from an elevated Administrator PowerShell prompt."
}

$devices = Get-ChildItem -Path $classPath | Where-Object { $_.PSChildName -match '^\d{4}$' }
$found = 0

foreach ($dev in $devices) {
    $props = Get-ItemProperty -Path $dev.PSPath
    if ($props.DriverDesc -match 'NVIDIA|2080 Ti|Turing' -or $props.ProviderName -eq 'NVIDIA') {
        $found++
        $cardName = $props.DriverDesc
        $current = $props.EnableGpuFirmware
        if ($Status) {
            Write-Host "Card [$($dev.PSChildName)] $cardName -> EnableGpuFirmware = $current"
            continue
        }
        if ($Disable) {
            if ($null -ne $current) {
                Remove-ItemProperty -Path $dev.PSPath -Name "EnableGpuFirmware" -Force
                Write-Host "[REMOVED] Card [$($dev.PSChildName)] $cardName -> EnableGpuFirmware removed" -ForegroundColor Yellow
            } else {
                Write-Host "[SKIP] Card [$($dev.PSChildName)] $cardName has no EnableGpuFirmware"
            }
        } else {
            Set-ItemProperty -Path $dev.PSPath -Name "EnableGpuFirmware" -Value 1 -Type DWord -Force
            Write-Host "[ENABLED] Card [$($dev.PSChildName)] $cardName -> EnableGpuFirmware = 1 (GSP mode enabled)" -ForegroundColor Green
        }
    }
}

if ($found -eq 0) {
    Write-Warning "No NVIDIA graphics cards found under display class registry."
}
