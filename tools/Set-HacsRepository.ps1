param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?$')]
    [string]$Owner,
    [ValidatePattern('^[A-Za-z0-9_.-]+$')]
    [string]$Repository = 'ONT-Watchdog'
)

$ErrorActionPreference = 'Stop'
$manifestPath = Join-Path (Split-Path $PSScriptRoot -Parent) 'custom_components\xtatech_watchdog\manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$manifest.documentation = "https://github.com/$Owner/$Repository"
$manifest.issue_tracker = "https://github.com/$Owner/$Repository/issues"
$manifest.codeowners = @("@$Owner")
$json = $manifest | ConvertTo-Json -Depth 10
[System.IO.File]::WriteAllText($manifestPath, $json + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
Write-Output "Repository metadata set to $Owner/$Repository. This does not publish anything."
