$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$manifest = Get-Content -LiteralPath (Join-Path $root 'custom_components\xtatech_watchdog\manifest.json') -Raw | ConvertFrom-Json
if ($manifest.documentation -match 'REPLACE_') { throw 'Set the real GitHub repository metadata first.' }
$apiSource = Get-Content -LiteralPath (Join-Path $root 'ONT_Watchdog_ESP8266_HA\HomeAssistantApi.h') -Raw
$firmwareVersion = [regex]::Match($apiSource, 'HA_FIRMWARE_VERSION = "([^"]+)"').Groups[1].Value
if (-not $firmwareVersion) { throw 'Firmware version was not found.' }
$buildPath = Join-Path $root ('builds\esp8266-ha-' + ($firmwareVersion -replace '-ha$', ''))
New-Item -ItemType Directory -Path $buildPath -Force | Out-Null
$firmware = Join-Path $root 'ONT_Watchdog_ESP8266_HA\.pio\build\esp12f\firmware.bin'
if (-not (Test-Path -LiteralPath $firmware)) { throw 'Compile the HA firmware first.' }
Copy-Item -LiteralPath $firmware -Destination (Join-Path $buildPath 'firmware.bin')
$metadata = [ordered]@{
    firmware_version = $firmwareVersion
    integration_version = $manifest.version
    api_version = 1
    firmware_sha256 = (Get-FileHash -LiteralPath $firmware -Algorithm SHA256).Hash
    source_directory = 'ONT_Watchdog_ESP8266_HA'
    repository = $manifest.documentation
}
$json = $metadata | ConvertTo-Json
[System.IO.File]::WriteAllText((Join-Path $buildPath 'build.json'), $json + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
$sourceFiles = Get-ChildItem -LiteralPath (Join-Path $root 'ONT_Watchdog_ESP8266_HA') -File
if (Test-Path -LiteralPath (Join-Path $root 'LICENSE')) { $sourceFiles += Get-Item -LiteralPath (Join-Path $root 'LICENSE') }
Compress-Archive -LiteralPath $sourceFiles.FullName -DestinationPath (Join-Path $buildPath 'firmware-source.zip') -Force
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archiveFiles = @(Get-ChildItem -LiteralPath (Join-Path $root 'custom_components') -Recurse -File | Where-Object { $_.FullName -notmatch '[\\/]__pycache__[\\/]' })
$archiveFiles += Get-Item -LiteralPath (Join-Path $root 'hacs.json'), (Join-Path $root 'README.md'), (Join-Path $root 'docs\HOME_ASSISTANT.md')
if (Test-Path -LiteralPath (Join-Path $root 'LICENSE')) { $archiveFiles += Get-Item -LiteralPath (Join-Path $root 'LICENSE') }
$integrationArchive = Join-Path $root ("builds\xtatech-watchdog-ha-integration-$($manifest.version).zip")
$archiveStream = [System.IO.File]::Open($integrationArchive, [System.IO.FileMode]::Create)
$archive = [System.IO.Compression.ZipArchive]::new($archiveStream, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in $archiveFiles) {
        $entryName = $file.FullName.Substring($root.Length + 1).Replace('\', '/')
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entryName)
    }
} finally {
    $archive.Dispose()
    $archiveStream.Dispose()
}
Write-Output "Packaged HA firmware in $buildPath and integration in $integrationArchive. Historical builds were not modified."
