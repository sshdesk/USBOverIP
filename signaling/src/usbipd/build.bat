@echo off
setlocal

 echo ===================================================
 echo Building USB/IP Server in C++ with CMake
 echo ===================================================

where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] CMake was not found in PATH.
    echo Install CMake or open a Visual Studio Developer Command Prompt with CMake available.
    exit /b 1
)

cmake -S . -B build -A x64
if errorlevel 1 exit /b %errorlevel%

cmake --build build --config Release --parallel
if errorlevel 1 exit /b %errorlevel%

echo ===================================================
echo [SUCCESS] Built executable: build\bin\usbip_server.exe
echo Run it with: build\bin\usbip_server.exe [port] --passthrough [BUSID] [VID] [PID]
echo ===================================================
