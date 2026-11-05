param(
    [string]$TokenId = "AB369739810346",
    [string]$RemoteUsb = "true",
    [string]$InstallDir = $PSScriptRoot
)

# Build the configuration file content
$configContent = @"
TOKEN_ID=$TokenId
REMOTE_USB=$RemoteUsb
LOG_LEVEL=info
"@

# Define target file path (inside bin folder)
$configPath = Join-Path $InstallDir "config.txt"

# Write the config file securely with UTF8 encoding
Set-Content -Path $configPath -Value $configContent -Encoding UTF8