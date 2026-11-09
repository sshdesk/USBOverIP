# ==============================================================================
# usbip-win2 Automated Uninstallation & Cleanup Script (Color Fixed)
# ==============================================================================

# 1. Define folder path and variables
$AppDir = Split-Path -Path $MyInvocation.MyCommand.Path -Parent
$HwId = "ROOT\USBIP_WIN2\UDE"
$UsbClassGuid = "{36fc9e60-c465-11cf-8056-444553540000}"

Set-Location -Path $AppDir

Write-Host "--- Starting usbip-win2 Driver Teardown ---" -ForegroundColor DarkYellow

# 2. Remove the Virtual Hardware Device Node topology
Write-Host "[1/4] Tearing down Virtual Root Device Nodes..." -ForegroundColor Cyan
if (Test-Path ".\devnode.exe") {
    .\devnode.exe remove $HwId
} else {
    Write-Host "Warning: devnode.exe missing. Attempting fallback via native PnP commands." -ForegroundColor Yellow
    Get-PnpDevice -HardwareID "*USBIP_WIN2*" -ErrorAction SilentlyContinue | ForEach-Object {
        pnputil.exe /remove-device $_.InstanceId
    }
}

# 3. Cleanly remove usbip2_filter from the global USB UpperFilters stack
Write-Host "[2/4] Scrubbing UpperFilters from Registry..." -ForegroundColor Cyan
$RegPath = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$UsbClassGuid"
$CurrentFilters = (Get-ItemProperty -Path $RegPath -Name "UpperFilters" -ErrorAction SilentlyContinue).UpperFilters

if ($CurrentFilters -contains "usbip2_filter") {
    $CleanedFilters = $CurrentFilters | Where-Object { $_ -ne "usbip2_filter" }
    if ($CleanedFilters) {
        Set-ItemProperty -Path $RegPath -Name "UpperFilters" -Value $CleanedFilters
    } else {
        Remove-ItemProperty -Path $RegPath -Name "UpperFilters"
    }
    Write-Host "Success: usbip2_filter removed from USB Controller Class Registry." -ForegroundColor Green
} else {
    Write-Host "Notice: usbip2_filter was not found in UpperFilters." -ForegroundColor Yellow
}

# 4. Uninstall and delete the driver packages from the Windows Driver Store
Write-Host "[3/4] Purging driver blueprints from Windows Driver Store (OEM INF)..." -ForegroundColor Cyan
Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -match "usbip2_" } | ForEach-Object {
    Write-Host "Deleting driver package: $($_.Driver)" -ForegroundColor Yellow
    pnputil.exe /delete-driver $_.Driver /uninstall /force
}

# 5. Trigger a Plug-and-Play rescan to apply adjustments and stop dead services
Write-Host "[4/4] Triggering hardware refresh to synchronize system..." -ForegroundColor Cyan
pnputil.exe /scan-devices
Start-Sleep -Seconds 2

# 6. Final verification checks
Write-Host "`n--- Verification Check ---" -ForegroundColor DarkYellow
$RemainingDevice = Get-PnpDevice -FriendlyName "*Emulated Host Controller*" -ErrorAction SilentlyContinue

if (-not $RemainingDevice) {
    Write-Host "Uninstallation Completed Successfully! All drivers and hardware structures wiped cleanly." -ForegroundColor Green
} else {
    Write-Host "Warning: Virtual Host Controller still listed. A system reboot may be required to finalize deletion." -ForegroundColor Red
}
