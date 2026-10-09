# USB/IP C Real Passthrough - PowerShell Installer
# Installs the EXISTING usbip_server.exe and the VBox USB driver components.
# No usbipd-win installation and no additional EXE are required.
#
# Expected layout:
#   install_usbip_passthrough.ps1
#   bin\usbip_server.exe
#   drivers\
#       VBoxUSB.inf
#       VBoxUSB.sys
#       VBoxUSB.cat
#       VBoxUSBMon.sys
#       (vboxusb.PNF is optional and is not installed separately)
#
# Run as Administrator, or the script will request elevation.

#Requires -Version 5.1
$ErrorActionPreference = "Stop"

$PackageRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ServerExe   = Join-Path $PackageRoot "bin\usbip_server.exe"
$DriverRoot  = Join-Path $PackageRoot "drivers"

$InstallDir  = Join-Path ${env:ProgramFiles} "USBIP-Passthrough"
$BinDir      = Join-Path $InstallDir "bin"
$InstalledDrivers = Join-Path $InstallDir "drivers"

$FirewallName = "USBIP Passthrough Server (TCP 3240)"
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

if (-not (Test-Path -LiteralPath $ServerExe -PathType Leaf)) {
    Fail "Missing existing server executable:"
    Write-Host "    $ServerExe"
    exit 1
}

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
#
# The working usbipd-win installation on this machine has VBoxUSBMon.sys but
# no VBoxUSBMon.inf/.cat in its Drivers directory. VBoxUSBMon's official INF
# defines a normal kernel service whose binary is %SystemRoot%\System32\
# VBoxUSBMon.sys. We therefore register the existing, already-working
# VBoxUSBMon.sys directly rather than inventing a CAT/INF pair.
# ---------------------------------------------------------------------------

$System32VBoxUSBMon = Join-Path $env:WINDIR "System32\VBoxUSBMon.sys"

Info "Installing VBoxUSBMon.sys..."

Copy-Item -LiteralPath $VBoxUSBMonSys -Destination $System32VBoxUSBMon -Force
Ok "VBoxUSBMon.sys copied to $System32VBoxUSBMon"

# Check whether the service already exists.
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

# Make sure the service points to the package we installed.
& sc.exe config VBoxUSBMon binPath= "`"$System32VBoxUSBMon`"" start= auto | Out-Null

# Start it if it isn't running.
$service = Get-Service -Name $VBoxUSBMonService -ErrorAction SilentlyContinue

if ($service -and $service.Status -ne "Running") {
    Info "Starting VBoxUSBMon..."
    & sc.exe start VBoxUSBMon 2>&1 | ForEach-Object {
        Write-Host "    $_"
    }

    if ($LASTEXITCODE -ne 0) {
        Warn "VBoxUSBMon could not be started immediately."
        Warn "This can happen when an existing USB filter/service still owns the driver."
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
# Install existing usbip_server.exe.
# ---------------------------------------------------------------------------

Info "Installing existing usbip_server.exe..."

New-Item -ItemType Directory -Force -Path $BinDir | Out-Null
New-Item -ItemType Directory -Force -Path $InstalledDrivers | Out-Null

Copy-Item -LiteralPath $ServerExe `
    -Destination (Join-Path $BinDir "usbip_server.exe") `
    -Force

# Copy driver files for reference/reinstall.
Copy-Item -LiteralPath $DriverRoot `
    -Destination $InstalledDrivers `
    -Recurse -Force

Ok "Existing usbip_server.exe installed."

# ---------------------------------------------------------------------------
# Add server directory to system PATH.
# ---------------------------------------------------------------------------

$machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
$entries = @()

if ($machinePath) {
    $entries = $machinePath -split ";" | Where-Object {
        $_ -and $_.Trim()
    }
}

$alreadyInPath = $entries | Where-Object {
    $_.TrimEnd("\") -ieq $BinDir.TrimEnd("\")
}

if (-not $alreadyInPath) {
    Info "Adding USB/IP server to system PATH..."
    $newPath = (($entries + $BinDir) -join ";")
    [Environment]::SetEnvironmentVariable("Path", $newPath, "Machine")
    Ok "PATH updated."
}
else {
    Ok "USB/IP server is already in PATH."
}

# ---------------------------------------------------------------------------
# Firewall TCP 3240.
# ---------------------------------------------------------------------------

Info "Configuring Windows Firewall..."

$rule = Get-NetFirewallRule -DisplayName $FirewallName -ErrorAction SilentlyContinue

if (-not $rule) {
    New-NetFirewallRule `
        -DisplayName $FirewallName `
        -Direction Inbound `
        -Action Allow `
        -Protocol TCP `
        -LocalPort 3240 `
        -Profile Any `
        -Description "USB/IP C Real Passthrough server TCP 3240" |
        Out-Null

    Ok "Firewall rule created."
}
else {
    Ok "Firewall rule already exists."
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

$InstalledExe = Join-Path $BinDir "usbip_server.exe"
$hash = Get-FileHash -LiteralPath $InstalledExe -Algorithm SHA256

Write-Host "==================================================" -ForegroundColor Green
Write-Host " Installation complete" -ForegroundColor Green
Write-Host "==================================================" -ForegroundColor Green
Write-Host ""
Write-Host "Server:"
Write-Host "  $InstalledExe"
Write-Host ""
Write-Host "SHA256:"
Write-Host "  $($hash.Hash)"
Write-Host ""
Write-Host "USB/IP port:"
Write-Host "  TCP 3240"
Write-Host ""
Write-Host "Start v25:"
Write-Host "  usbip_server.exe 3240 --passthrough 3-2 0951 16A1"
Write-Host ""
Write-Host "Force release:"
Write-Host "  usbip_server.exe --force-release 3-2"
Write-Host ""
Write-Host "IMPORTANT:"
Write-Host "  The VBoxUSBMon.sys file is copied from your existing working"
Write-Host "  installation. The script does NOT install usbipd-win."
Write-Host ""

Ok "Done."
