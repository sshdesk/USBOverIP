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

