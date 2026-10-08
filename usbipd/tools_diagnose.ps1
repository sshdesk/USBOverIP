# Run from an elevated PowerShell prompt.
Write-Host "=== usbipd-win / VBoxUSB diagnostics ===" -ForegroundColor Cyan
usbipd --version
Write-Host "`n--- usbipd service ---"
sc.exe query usbipd
Write-Host "`n--- VBoxUSBMon service ---"
sc.exe qc VBoxUSBMon
sc.exe query VBoxUSBMon
Write-Host "`n--- VBox driver files ---"
Get-ChildItem "$env:ProgramFiles\usbipd-win\Drivers" -ErrorAction SilentlyContinue |
    Select-Object Name,Length,LastWriteTime
Write-Host "`n--- VBox kernel drivers ---"
Get-CimInstance Win32_SystemDriver |
    Where-Object { $_.Name -match '^VBoxUSB' } |
    Select-Object Name,DisplayName,PathName,State,Started
Write-Host "`n--- Monitor DOS device ---"
Test-Path "\\.\VBoxUSBMon"
Write-Host "`n--- USB devices ---"
usbipd list
