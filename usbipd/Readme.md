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


