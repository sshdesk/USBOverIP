# Requires -RunAsAdministrator

# Ensure script is executed with Administrator privileges
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "[-] This script must be run as Administrator!"
    Exit 1
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
if (-not $ScriptDir) {
    $ScriptDir = Get-Location
}

# Required Driver Package Files
$VBoxUsbSys    = Join-Path $ScriptDir "VBoxUSB.sys"
$VBoxUsbInf    = Join-Path $ScriptDir "VBoxUSB.inf"
$VBoxUsbCat    = Join-Path $ScriptDir "VBoxUSB.cat"
$VBoxUsbMonSys = Join-Path $ScriptDir "VBoxUSBMon.sys"

$System32DriversDir = "$env:SystemRoot\System32\drivers"

Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "  VirtualBox USB Driver & Service Installer" -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan

# 1. Verify all package files are present
$RequiredFiles = @($VBoxUsbSys, $VBoxUsbInf, $VBoxUsbCat, $VBoxUsbMonSys)
foreach ($file in $RequiredFiles) {
    if (-not (Test-Path $file)) {
        Write-Error "[-] Missing required file: '$file'. All .sys, .inf, and .cat files must be in '$ScriptDir'."
        Exit 1
    }
}

# 2. Copy binaries to C:\Windows\System32\drivers
Write-Host "[*] Copying driver binaries to System32\drivers..." -ForegroundColor Yellow
Copy-Item -Path $VBoxUsbSys -Destination $System32DriversDir -Force
Copy-Item -Path $VBoxUsbMonSys -Destination $System32DriversDir -Force

# 3. Register VBoxUSB.inf and VBoxUSB.cat into the Windows Driver Store (PnP Store)
Write-Host "`n[*] Registering INF and Catalog into Windows Driver Store..." -ForegroundColor Yellow
$pnputilResult = pnputil.exe /add-driver "$VBoxUsbInf" /install
Write-Host $pnputilResult

# 4. Configure & Start VBoxUSBMon Service
Write-Host "`n[*] Configuring VBoxUSBMon service..." -ForegroundColor Yellow
$monSvc = sc.exe query VBoxUSBMon 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "[*] Creating VBoxUSBMon service..." -ForegroundColor Gray
    sc.exe create VBoxUSBMon type= kernel start= demand error= normal binPath= "system32\drivers\VBoxUSBMon.sys" displayName= "VirtualBox USB Monitor Driver"
}
sc.exe start VBoxUSBMon | Out-Null

# 5. Configure & Start VBoxUSB Service
Write-Host "`n[*] Configuring VBoxUSB service..." -ForegroundColor Yellow
$usbSvc = sc.exe query VBoxUSB 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "[*] Creating VBoxUSB service..." -ForegroundColor Gray
    sc.exe create VBoxUSB type= kernel start= demand error= normal binPath= "system32\drivers\VBoxUSB.sys" displayName= "VirtualBox USB Service" group= "Base"
}

# Registry configuration required by VBoxUSB INF
$regPath = "HKLM:\System\CurrentControlSet\Services\VBoxUSB"
if (Test-Path $regPath) {
    Set-ItemProperty -Path $regPath -Name "DevLoader" -Value "*ntkern" -Type String
    Set-ItemProperty -Path $regPath -Name "NTMPDriver" -Value "VBoxUSB.sys" -Type String
}

sc.exe start VBoxUSB | Out-Null

Write-Host "`n==================================================" -ForegroundColor Cyan
Write-Host "[+] Driver Package & Services Installed Successfully!" -ForegroundColor Green
Write-Host "==================================================" -ForegroundColor Cyan
