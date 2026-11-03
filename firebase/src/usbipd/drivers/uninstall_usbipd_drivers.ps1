# USB/IP C Real Passthrough - PowerShell Uninstaller
# Removes the custom server installation and installer-created configuration.
# Run as Administrator, or the script will request elevation.
#
# SAFETY:
# - Does NOT remove the Windows Driver Store VBoxUSB package (oem*.inf).
# - Does NOT uninstall VirtualBox or usbipd-win.
# - Removes VBoxUSBMon service only if it points to the installer's System32 copy.
# - Use -RemoveCustomDriverFiles to remove the custom Program Files folder too.

#Requires -Version 5.1
param([switch]$RemoveCustomDriverFiles)

$ErrorActionPreference = "Stop"
$InstallDir = Join-Path ${env:ProgramFiles} "USBIP"
$InstalledMonSys = Join-Path $env:WINDIR "System32\VBoxUSBMon.sys"
$ServiceName = "VBoxUSBMon"

function Is-Administrator {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
function Info($s) { Write-Host "[*] $s" -ForegroundColor Cyan }
function Ok($s)   { Write-Host "[+] $s" -ForegroundColor Green }
function Warn($s) { Write-Host "[!] $s" -ForegroundColor Yellow }
function Fail($s) { Write-Host "[X] $s" -ForegroundColor Red }

if (-not (Is-Administrator)) {
    Info "Requesting Administrator privileges..."
    try {
        $args = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '"'
        if ($RemoveCustomDriverFiles) { $args += ' -RemoveCustomDriverFiles' }
        Start-Process powershell.exe -Verb RunAs -ArgumentList $args -Wait
        exit $LASTEXITCODE
    } catch {
        Fail "Could not obtain Administrator privileges."
        Write-Host $_.Exception.Message
        exit 1
    }
}

Write-Host ""
Write-Host "USB/IP C Real Passthrough Uninstaller" -ForegroundColor White
Write-Host "-------------------------------------" -ForegroundColor White
Warn "This removes the custom setup only; it does not uninstall VirtualBox or usbipd-win."
Write-Host ""

# Remove installer-created firewall rules.
$FirewallRulesToRemove = @(
    "USBIP Passthrough Server (TCP 3240)",
    "USBIP GUI Electron (All TCP In)",
    "USBIP GUI Electron (All UDP In)",
    "USBIP GUI Win32 (All TCP In)",
    "USBIP GUI Win32 (All UDP In)"
)

foreach ($ruleName in $FirewallRulesToRemove) {
    try {
        $rules = Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue
        if ($rules) {
            $rules | Remove-NetFirewallRule
            Ok "Removed firewall rule: $ruleName"
        }
    } catch { Warn "Could not remove firewall rule $ruleName : $($_.Exception.Message)" }
}

# Remove VBoxUSBMon service only if its binary path matches our installer's copy.
try {
    $svc = Get-CimInstance Win32_SystemDriver -Filter "Name='VBoxUSBMon'" -ErrorAction SilentlyContinue
    if ($svc) {
        $servicePath = [Environment]::ExpandEnvironmentVariables([string]$svc.PathName).Trim('"')
        $normalized = $servicePath.Replace("\??\", "").Replace("\\?\", "")
        if ($normalized -ieq $InstalledMonSys) {
            Warn "VBoxUSBMon service points to the installer-managed file: $servicePath"
            & sc.exe stop $ServiceName 2>&1 | ForEach-Object { Write-Host "    $_" }
            & sc.exe delete $ServiceName 2>&1 | ForEach-Object { Write-Host "    $_" }
            Ok "Requested removal of VBoxUSBMon service registration."
            if (Test-Path -LiteralPath $InstalledMonSys) {
                try {
                    Remove-Item -LiteralPath $InstalledMonSys -Force
                    Ok "Removed copied VBoxUSBMon.sys."
                } catch { Warn "Could not delete driver binary (it may be in use): $($_.Exception.Message)" }
            }
        } else {
            Warn "VBoxUSBMon points to '$servicePath', not '$InstalledMonSys'. Leaving it untouched."
        }
    } else { Info "VBoxUSBMon service not found." }
} catch { Warn "Could not clean up VBoxUSBMon service: $($_.Exception.Message)" }

# Remove custom install folder if present or requested.
if ($RemoveCustomDriverFiles) {
    if (Test-Path -LiteralPath $InstallDir) {
        try {
            Remove-Item -LiteralPath $InstallDir -Recurse -Force
            Ok "Removed $InstallDir"
        } catch { Warn "Could not completely remove install directory: $($_.Exception.Message)" }
    } else { Info "Custom install directory not found." }
} else {
    Warn "Kept $InstallDir and its copied files."
    Write-Host "To remove that folder too, run:"
    Write-Host "  .\uninstall_usbipd_drivers.ps1 -RemoveCustomDriverFiles"
}

Write-Host ""
Write-Host "Uninstall steps finished." -ForegroundColor Green
Write-Host "The VBoxUSB Driver Store package (oem*.inf) was intentionally retained because it may be shared with other software."