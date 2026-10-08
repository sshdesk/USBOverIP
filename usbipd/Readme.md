# USB/IP C Server 

Real Windows USB passthrough using VBoxUSB/VBoxUSBMon

## Run

```powershell
.\build.bat
.\bin\usbip_server.exe 3240 --passthrough 3-2 0951 16A1
```

## Force release

From a second elevated PowerShell window:

```powershell
.\bin\usbip_server.exe --force-release 3-2
```

This tells the running server to terminate the USB/IP session and perform its normal USB cleanup before exiting.

## List devices

```powershell
.\bin\usbip_server.exe --list
```


# Clean or regenerate the build configuration
cmake -B build -G "Visual Studio 17 2022" -A x64

# Compile the newly named target layout configuration
cmake --build build --config Debug

go solution file sln properties -> Linker -> ALL option -> UAC Execution Level
requireAdministrator (/level='requireAdministrator')



sc.exe  query usbipd

sc.exe query VBoxUsbMon


>> Get-CimInstance Win32_Service -Filter "Name = 'VBoxUsbMon'" | Select-Object Name, DisplayName, PathName, StartMode
PS C:\>
>> Get-Service VBoxUsbMon | Select-Object Name, Status, StartType

Name        Status StartType
----        ------ ---------
VBoxUsbMon Running    Manual





PS C:\>
>> Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\VBoxUsbMon | Select-Object ImagePath, Start, Type

ImagePath                                              Start Type
---------                                              ----- ----
\??\C:\Program Files\usbipd-win\Drivers\VBoxUSBMon.sys     3    1




Set-ExecutionPolicy Bypass -Scope Process -Force
.\install.ps1