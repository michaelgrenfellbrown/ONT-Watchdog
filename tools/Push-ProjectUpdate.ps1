param([string]$Message = 'Complete GPL-3.0 licensing and HACS installation packages')

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $root
. (Join-Path $PSScriptRoot 'Check-GitHubAccess.ps1') -Owner 'michaelgrenfellbrown'
$paths = @('README.md', 'docs/HOME_ASSISTANT.md', 'LICENSE', 'tools', 'builds', 'ONT_Watchdog_ESP8266', 'ONT_Watchdog_ESP8266_HA', 'custom_components', 'tests')
& git -c "safe.directory=$root" add -- @paths
if ($LASTEXITCODE -ne 0) { throw 'Staging failed.' }
$commitName = if ($githubProfile.name) { $githubProfile.name } else { $githubProfile.login }
$commitEmail = "$($githubProfile.id)+$($githubProfile.login)@users.noreply.github.com"
& git -c "safe.directory=$root" -c "user.name=$commitName" -c "user.email=$commitEmail" commit --only -m $Message -- @paths
if ($LASTEXITCODE -ne 0) { throw 'Commit failed. Nothing was pushed.' }
& git -c "safe.directory=$root" push
if ($LASTEXITCODE -ne 0) { throw 'Push failed. The local commit is preserved.' }
