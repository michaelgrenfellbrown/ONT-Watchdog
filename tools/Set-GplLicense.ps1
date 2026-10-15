$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$response = Invoke-WebRequest -UseBasicParsing -Uri 'https://www.gnu.org/licenses/gpl-3.0.txt' -TimeoutSec 30
$licenseText = [string]$response.Content
if (-not $licenseText.Contains('GNU GENERAL PUBLIC LICENSE') -or -not $licenseText.Contains('Version 3, 29 June 2007') -or $licenseText.Length -lt 30000) {
    throw 'The canonical GPL-3.0 license text could not be verified.'
}
[System.IO.File]::WriteAllText((Join-Path $root 'LICENSE'), $licenseText, [System.Text.UTF8Encoding]::new($false))
Write-Output 'Installed the canonical GPL-3.0 license text from GNU.'
