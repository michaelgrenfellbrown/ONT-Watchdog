$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$manifest = Get-Content -LiteralPath (Join-Path $root 'custom_components\xtatech_watchdog\manifest.json') -Raw | ConvertFrom-Json
if ($manifest.documentation -match 'REPLACE_') { throw 'Set the real GitHub repository metadata first.' }
$buildPath = Join-Path $root 'builds\esp8266-ha-1.1.0'
New-Item -ItemType Directory -Path $buildPath -Force | Out-Null
$firmware = Join-Path $root 'ONT_Watchdog_ESP8266_HA\.pio\build\esp12f\firmware.bin'
if (-not (Test-Path -LiteralPath $firmware)) { throw 'Compile the HA firmware first.' }
Copy-Item -LiteralPath $firmware -Destination (Join-Path $buildPath 'firmware.bin')
$metadata = [ordered]@{
    firmware_version = '1.1.0-ha'
    integration_version = $manifest.version
    api_version = 1
    firmware_sha256 = (Get-FileHash -LiteralPath $firmware -Algorithm SHA256).Hash
    source_directory = 'ONT_Watchdog_ESP8266_HA'
    repository = $manifest.documentation
}
$json = $metadata | ConvertTo-Json
[System.IO.File]::WriteAllText((Join-Path $buildPath 'build.json'), $json + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
$sourceFiles = Get-ChildItem -LiteralPath (Join-Path $root 'ONT_Watchdog_ESP8266_HA') -File
Compress-Archive -LiteralPath $sourceFiles.FullName -DestinationPath (Join-Path $buildPath 'firmware-source.zip') -Force
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archiveFiles = @(Get-ChildItem -LiteralPath (Join-Path $root 'custom_components') -Recurse -File | Where-Object { $_.FullName -notmatch '[\\/]__pycache__[\\/]' })
$archiveFiles += Get-Item -LiteralPath (Join-Path $root 'hacs.json'), (Join-Path $root 'README.md'), (Join-Path $root 'docs\HOME_ASSISTANT.md')
$archiveStream = [System.IO.File]::Open((Join-Path $root 'builds\xtatech-watchdog-ha-integration-1.0.0.zip'), [System.IO.FileMode]::Create)
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
Compress-Archive -LiteralPath (Join-Path $root 'builds\esp8266-standalone-2026-10-15\ONT_Watchdog_ESP8266'), (Join-Path $root 'builds\esp8266-standalone-2026-10-15\firmware.bin'), (Join-Path $root 'builds\esp8266-standalone-2026-10-15\BUILD.md') -DestinationPath (Join-Path $root 'builds\esp8266-standalone-2026-10-15.zip') -Force
Write-Output "Packaged HA firmware and integration in $buildPath and builds\xtatech-watchdog-ha-integration-1.0.0.zip"
