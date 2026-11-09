PowerShell.exe -ExecutionPolicy Bypass -File .\uninstall_usbip_drivers.ps1

PowerShell.exe -ExecutionPolicy Bypass -File .\install_usbip_drivers.ps1

To test driver 

 ./usbip.exe port


Create the hardware node via devnode.exe check ps1 script



PnPUtil 

pnputil.exe /add-driver usbip2_filter.inf /install




sc.exe stop usbip2_ude

sc.exe delete usbip2_ude

sc.exe query usbip2_ude



sc.exe query usbip2_filter

sc.exe stop usbip2_filter

sc.exe delete usbip2_filter

pnputil /delete-driver usbip2_ude.inf /uninstall /force

pnputil /delete-driver usbip2_filter.inf /uninstall /force


pnputil /enum-drivers

pnputil /delete-driver oem14.inf /uninstall /force

pnputil /delete-driver oem44.inf /uninstall /force
pnputil /delete-driver oem18.inf /uninstall /force

Published Name:     oem44.inf
Original Name:      usbip2_ude.inf
Provider Name:      USBIP-WIN2
Class Name:         Universal Serial Bus controllers
Class GUID:         {36fc9e60-c465-11cf-8056-444553540000}
Driver Version:     09/22/2026 12.1.30.403
Signer Name:        Microsoft Windows Hardware Compatibility Publisher

Published Name:     oem18.inf
Original Name:      usbip_root.inf
Provider Name:      usbip-win project
Class Name:         System devices
Class GUID:         {4d36e97d-e325-11ce-bfc1-08002be10318}
Driver Version:     06/18/2022 0.3.6.0
Signer Name:        Microsoft Windows Hardware Compatibility Publisher




for trouble shooting if your USB 3.0 extended controller does not work


$path = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\{36fc9e60-c465-11cf-8056-444553540000}"

# Remove UpperFilters if it exists
if (Get-ItemProperty -Path $path -Name "UpperFilters" -ErrorAction SilentlyContinue) {
    Remove-ItemProperty -Path $path -Name "UpperFilters" -Verbose
} else {
    Write-Host "UpperFilters does not exist. No action needed." -ForegroundColor Yellow
}

# Remove LowerFilters if it exists
if (Get-ItemProperty -Path $path -Name "LowerFilters" -ErrorAction SilentlyContinue) {
    Remove-ItemProperty -Path $path -Name "LowerFilters" -Verbose
} else {
    Write-Host "LowerFilters does not exist. No action needed." -ForegroundColor Yellow
}




