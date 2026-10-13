$ErrorActionPreference = "Stop" # Stop the script execution on errors

Write-Host "============ Starting SDL GUI Build (PowerShell) ============" -ForegroundColor Cyan

# 1. Setup Build and Asset Directories
$ProjectRoot = Get-Location
$BuildDir = Join-Path $ProjectRoot "build"
$AssetsDir = Join-Path $ProjectRoot "assets"

if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir | Out-Null }
if (-not (Test-Path $AssetsDir)) { New-Item -ItemType Directory -Path $AssetsDir | Out-Null }

# 2. Fetch a fallback font if missing
$TargetFont = Join-Path $AssetsDir "arial.ttf"
if (-not (Test-Path $TargetFont)) {
    Write-Host "No font asset found. Pulling standard system Arial..."
    $SystemFont = "C:\Windows\Fonts\arial.ttf"
    if (Test-Path $SystemFont) {
        Copy-Item $SystemFont $TargetFont
    } else {
        Write-Warning "Windows system font path missing. Please map your target font to assets/arial.ttf manually."
    }
}

# 3. Configure and Build Code via CMake
Set-Location $BuildDir
Write-Host "Configuring CMake project..." -ForegroundColor Green
# If your SDL libs are elsewhere, append: -DCMAKE_PREFIX_PATH="C:\YourSDLFolder"
cmake .. 

Write-Host "Compiling application..." -ForegroundColor Green
cmake --build . --config Debug

# 4. Copy Assets directly next to the Debug output executable folder
Write-Host "Deploying assets to output target folder..." -ForegroundColor Green
$DebugOutputDir = Join-Path $BuildDir "Debug"

# Visual Studio creates a Debug/ folder by default
if (Test-Path $DebugOutputDir) {
    $TargetDebugAssets = Join-Path $DebugOutputDir "assets"
    if (-not (Test-Path $TargetDebugAssets)) { New-Item -ItemType Directory -Path $TargetDebugAssets | Out-Null }
    Copy-Item $TargetFont $TargetDebugAssets
    Write-Host "Successfully deployed assets directory inside build/Debug/!" -ForegroundColor Yellow
} else {
    # Fallback structure for single-target Ninja/MinGW configurations
    $TargetLocalAssets = Join-Path $BuildDir "assets"
    if (-not (Test-Path $TargetLocalAssets)) { New-Item -ItemType Directory -Path $TargetLocalAssets | Out-Null }
    Copy-Item $TargetFont $TargetLocalAssets
    Write-Host "Successfully deployed assets directory inside build/!" -ForegroundColor Yellow
}

Set-Location $ProjectRoot
Write-Host "============ Build Finished Successfully! ============" -ForegroundColor Cyan

