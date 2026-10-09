# ==============================================================================
# usbip-win2 Native CLI Deployment Script (Zero Dollar-Signs Version)
# ==============================================================================

Set-Location -Path (Split-Path -Path (Get-Variable -Name MyInvocation).Value.MyCommand.Path -Parent)

Write-Host "--- Starting usbip-win2 Driver Provisioning ---" -ForegroundColor Cyan

# 1. Stage the drivers into the Windows Driver Store
Write-Host "[1/5] Staging UDE and Filter drivers into system..." -ForegroundColor Cyan
pnputil.exe /add-driver .\usbip2_ude.inf /install
pnputil.exe /add-driver .\usbip2_filter.inf /install

# 2. Create the Virtual Hardware Root Device Node using the verified installer syntax
Write-Host "[2/5] Instantiating Virtual Root Device Node..." -ForegroundColor Cyan
.\devnode.exe install .\usbip2_ude.inf ROOT\USBIP_WIN2\UDE


# 4. Refresh the hardware layer to wake the SCM and start the services
Write-Host "[4/5] Triggering Plug-and-Play hardware rescan..." -ForegroundColor Cyan
pnputil.exe /scan-devices
Start-Sleep -Seconds 2 

# 5. Final verification checks
Write-Host "[5/5] Checking service and hardware statuses..." -ForegroundColor Cyan

Write-Host "`n--- Service States ---" -ForegroundColor Yellow
sc.exe query usbip2_ude | Select-String "STATE"
sc.exe query usbip2_filter | Select-String "STATE"

Write-Host "`n--- PnP Hardware Controller Detection ---" -ForegroundColor Yellow
Get-PnpDevice -FriendlyName "*Emulated Host Controller*" -ErrorAction SilentlyContinue | Format-Table -AutoSize
