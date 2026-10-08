@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo Building USB/IP Server in C (Visual Studio 2022 / MSVC)
echo ===================================================

REM Check if cl is available in PATH
where cl >nul 2>nul
if %errorlevel% neq 0 (
    echo [INFO] Searching for Visual Studio 2022 vcvars64.bat...
    set "VS_PATH=C:\Program Files\Microsoft Visual Studio\2022"
    set "VCVARS="
    
    if exist "!VS_PATH!\Community\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS=!VS_PATH!\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "!VS_PATH!\Professional\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS=!VS_PATH!\Professional\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "!VS_PATH!\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS=!VS_PATH!\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "!VS_PATH!\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS=!VS_PATH!\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    )

    if defined VCVARS (
        echo [INFO] Found: "!VCVARS!"
        call "!VCVARS!"
    ) else (
        echo [ERROR] Could not find vcvars64.bat automatically.
        echo Please run this from 'x64 Native Tools Command Prompt for VS 2022'.
        exit /b 1
    )
)

if not exist bin mkdir bin

cl /nologo /W4 /O2 /Iinclude src\main.c src\usb_device_hid.c src\usb_device_passthrough.c src\usbip_server.c /Fe:bin\usbip_server.exe /link ws2_32.lib advapi32.lib setupapi.lib cfgmgr32.lib

if %errorlevel% equ 0 (
    echo ===================================================
    echo [SUCCESS] Built executable: bin\usbip_server.exe
    echo Run it with: bin\usbip_server.exe [port]
    echo ===================================================
) else (
    echo [ERROR] Compilation failed!
    exit /b %errorlevel%
)
