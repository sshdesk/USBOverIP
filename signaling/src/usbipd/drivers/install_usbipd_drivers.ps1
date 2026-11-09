# USB/IP C Real Passthrough - PowerShell Installer
# Installs the VBox USB driver components without requiring an external server executable.
# No usbipd-win installation is required.
#
# Expected layout:
#   install_usbipd_drivers.ps1 (located inside the drivers directory)
#   VBoxUSB.inf
#   VBoxUSB.sys
#   VBoxUSB.cat
#   VBoxUSBMon.sys
#   (vboxusb.PNF is optional and is not installed separately)
#
# Run as Administrator, or the script will request elevation.

#Requires -Version 5.1
$ErrorActionPreference = "Stop"

$DriverRoot  = Split-Path -Parent $MyInvocation.MyCommand.Path

$BinPathElectron = Join-Path ${env:ProgramFiles} "USBIP\bin\guiElectron.exe"
$BinPathUsbip    = Join-Path ${env:ProgramFiles} "USBIP\bin\gui_usbip.exe"

$VBoxUSBMonService = "VBoxUSBMon"

function Info($s) { Write-Host "[*] $s" -ForegroundColor Cyan }
function Ok($s)   { Write-Host "[+] $s" -ForegroundColor Green }
function Warn($s) { Write-Host "[!] $s" -ForegroundColor Yellow }
function Fail($s) { Write-Host "[X] $s" -ForegroundColor Red }

function Is-Administrator {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Elevate automatically.
if (-not (Is-Administrator)) {
    Info "Requesting Administrator privileges..."
    try {
        $args = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '"'
        Start-Process powershell.exe -Verb RunAs -ArgumentList $args -Wait
        exit $LASTEXITCODE
    }
    catch {
        Fail "Could not obtain Administrator privileges."
        Write-Host $_.Exception.Message
        exit 1
    }
}

Write-Host ""
Write-Host "==================================================" -ForegroundColor White
Write-Host " USB/IP C Real Passthrough Installer" -ForegroundColor White
Write-Host "==================================================" -ForegroundColor White
Write-Host ""

# ---------------------------------------------------------------------------
# Validate package
# ---------------------------------------------------------------------------

if (-not (Test-Path -LiteralPath $DriverRoot -PathType Container)) {
    Fail "Missing drivers directory:"
    Write-Host "    $DriverRoot"
    exit 1
}

$VBoxUSBInf = Join-Path $DriverRoot "VBoxUSB.inf"
$VBoxUSBSys = Join-Path $DriverRoot "VBoxUSB.sys"
$VBoxUSBCat = Join-Path $DriverRoot "VBoxUSB.cat"
$VBoxUSBMonSys = Join-Path $DriverRoot "VBoxUSBMon.sys"

foreach ($f in @($VBoxUSBInf, $VBoxUSBSys, $VBoxUSBCat, $VBoxUSBMonSys)) {
    if (-not (Test-Path -LiteralPath $f -PathType Leaf)) {
        Fail "Missing required driver file:"
        Write-Host "    $f"
        exit 1
    }
}

# ---------------------------------------------------------------------------
# Display package information
# ---------------------------------------------------------------------------

$os = Get-CimInstance Win32_OperatingSystem
Info "Windows: $($os.Caption) $($os.Version)"
Info "Architecture: $env:PROCESSOR_ARCHITECTURE"

# ---------------------------------------------------------------------------
# Install VBoxUSB using the signed INF/CAT package.
# ---------------------------------------------------------------------------

$PnPUtil = Join-Path $env:WINDIR "System32\pnputil.exe"

if (-not (Test-Path $PnPUtil)) {
    Fail "pnputil.exe was not found."
    exit 1
}

Info "Installing VBoxUSB driver package..."

$pnputilOutput = & $PnPUtil /add-driver "`"$VBoxUSBInf`"" /install 2>&1
$pnputilExit = $LASTEXITCODE

$pnputilOutput | ForEach-Object {
    Write-Host "    $_"
}

if ($pnputilExit -ne 0) {
    Fail "VBoxUSB driver installation failed (exit code $pnputilExit)."
    exit $pnputilExit
}

Ok "VBoxUSB driver package installed."

# ---------------------------------------------------------------------------
# Install VBoxUSBMon.
# ---------------------------------------------------------------------------

$System32VBoxUSBMon = Join-Path $env:WINDIR "System32\VBoxUSBMon.sys"

Info "Installing VBoxUSBMon.sys..."

Copy-Item -LiteralPath $VBoxUSBMonSys -Destination $System32VBoxUSBMon -Force
Ok "VBoxUSBMon.sys copied to $System32VBoxUSBMon"

$service = Get-Service -Name $VBoxUSBMonService -ErrorAction SilentlyContinue

if (-not $service) {
    Info "Creating VBoxUSBMon kernel service..."

    & sc.exe create VBoxUSBMon `
        type= kernel `
        start= auto `
        error= normal `
        binPath= "`"$System32VBoxUSBMon`"" `
        DisplayName= "VirtualBox USB Monitor Service" | ForEach-Object {
            Write-Host "    $_"
        }

    if ($LASTEXITCODE -ne 0) {
        Fail "Could not create VBoxUSBMon service."
        exit 1
    }

    Ok "VBoxUSBMon service created."
}
else {
    Ok "VBoxUSBMon service already exists."
}

& sc.exe config VBoxUSBMon binPath= "`"$System32VBoxUSBMon`"" start= auto | Out-Null

$service = Get-Service -Name $VBoxUSBMonService -ErrorAction SilentlyContinue

if ($service -and $service.Status -ne "Running") {
    Info "Starting VBoxUSBMon..."
    & sc.exe start VBoxUSBMon 2>&1 | ForEach-Object {
        Write-Host "    $_"
    }

    if ($LASTEXITCODE -ne 0) {
        Warn "VBoxUSBMon could not be started immediately."
        Warn "A reboot may be required."
    }
    else {
        Ok "VBoxUSBMon is running."
    }
}
else {
    Ok "VBoxUSBMon is already running."
}

# ---------------------------------------------------------------------------
# Firewall Configuration for All TCP and UDP Ports (guiElectron & gui_usbip)
# ---------------------------------------------------------------------------

Info "Configuring Windows Firewall for all TCP and UDP ports..."

$Binaries = @(
    @{ Name = "USBIP GUI Electron (All TCP In)"; Path = $BinPathElectron; Proto = "TCP" },
    @{ Name = "USBIP GUI Electron (All UDP In)"; Path = $BinPathElectron; Proto = "UDP" },
    @{ Name = "USBIP GUI Win32 (All TCP In)"; Path = $BinPathUsbip; Proto = "TCP" },
    @{ Name = "USBIP GUI Win32 (All UDP In)"; Path = $BinPathUsbip; Proto = "UDP" }
)

foreach ($bin in $Binaries) {
    if (Test-Path $bin.Path) {
        $rule = Get-NetFirewallRule -DisplayName $bin.Name -ErrorAction SilentlyContinue
        if (-not $rule) {
            New-NetFirewallRule `
                -DisplayName $bin.Name `
                -Direction Inbound `
                -Program $bin.Path `
                -Protocol $bin.Proto `
                -Action Allow `
                -Profile Any `
                -Description "Allow all inbound $($bin.Proto) traffic for $(Split-Path $bin.Path -Leaf)" | Out-Null
            Ok "Created firewall rule: $($bin.Name)"
        } else {
            Ok "Firewall rule already exists: $($bin.Name)"
        }
    }
}

# ---------------------------------------------------------------------------
# Verification
# ---------------------------------------------------------------------------

Write-Host ""
Info "Verifying installed drivers..."

driverquery /v | findstr /i "VBoxUSB" | ForEach-Object {
    Write-Host "    $_"
}

Write-Host ""
Write-Host "==================================================" -ForegroundColor Green
Write-Host " Installation complete" -ForegroundColor Green
Write-Host "==================================================" -ForegroundColor Green
Write-Host ""
Ok "Done."