param(
    [string]$Owner = 'michaelgrenfellbrown',
    [string]$Repository = 'ONT-Watchdog'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $root
. (Join-Path $PSScriptRoot 'Check-GitHubAccess.ps1') -Owner $Owner
$repoUrl = "https://github.com/$Owner/$Repository.git"

function Invoke-ProjectGit {
    & git -c "safe.directory=$root" @args
    if ($LASTEXITCODE -ne 0) { throw 'A Git operation failed; publication stopped without forcing any update.' }
}

# Refuse to replace an existing remote repository or disturb staged user work.
$staged = Invoke-ProjectGit diff --cached --name-only
if ($staged) { throw 'There are pre-existing staged files. Review them before publishing.' }
$remoteNames = @(Invoke-ProjectGit remote)
$existingRemote = if ($remoteNames -contains 'origin') { Invoke-ProjectGit remote get-url origin } else { $null }
if ($existingRemote -and $existingRemote -ne $repoUrl) { throw 'An unrelated origin is already configured.' }
$existingRepository = $null
try {
    $existingRepository = Invoke-RestMethod -Uri "https://api.github.com/repos/$Owner/$Repository" -Headers $headers -TimeoutSec 15
} catch {
    if ([int]$_.Exception.Response.StatusCode -ne 404) { throw 'Could not check the target GitHub repository.' }
}
if ($existingRepository) {
    if ($existingRepository.private) { throw 'The existing repository is private; its visibility was not changed.' }
    $remoteRefs = & git ls-remote $repoUrl
    if ($LASTEXITCODE -ne 0 -or $remoteRefs) { throw 'The target repository already contains Git history or could not be checked.' }
} else {
    $payload = @{
        name = $Repository
        description = 'Xtatech ESP8266 ONT/router watchdog firmware and local Home Assistant HACS integration'
        private = $false
        has_issues = $true
        auto_init = $false
    } | ConvertTo-Json
    try {
        $created = Invoke-RestMethod -Method Post -Uri 'https://api.github.com/user/repos' -Headers $headers -ContentType 'application/json' -Body $payload -TimeoutSec 20
    } catch {
        throw 'GitHub repository creation failed; local work remains available.'
    }
}

$branch = 'codex/home-assistant-integration'
Invoke-ProjectGit checkout -b $branch
Invoke-ProjectGit add -- .github .gitignore README.md hacs.json requirements-dev.txt tests custom_components docs/HOME_ASSISTANT.md ONT_Watchdog ONT_Watchdog_ESP8266 ONT_Watchdog_ESP8266_HA tools builds
$commitName = if ($githubProfile.name) { $githubProfile.name } else { $Owner }
$commitEmail = "$($githubProfile.id)+$Owner@users.noreply.github.com"
& git -c "safe.directory=$root" -c "user.name=$commitName" -c "user.email=$commitEmail" commit -m 'Preserve standalone build and add Home Assistant firmware and HACS integration'
if ($LASTEXITCODE -ne 0) { throw 'Commit failed. The repository was not pushed.' }
if (-not $existingRemote) { Invoke-ProjectGit remote add origin $repoUrl }
Invoke-ProjectGit push -u origin $branch
$topics = @{ names = @('home-assistant', 'hacs', 'esp8266', 'watchdog', 'xtatech') } | ConvertTo-Json
try {
    $null = Invoke-RestMethod -Method Put -Uri "https://api.github.com/repos/$Owner/$Repository/topics" -Headers $headers -ContentType 'application/json' -Body $topics -TimeoutSec 15
    $defaultBranch = @{ default_branch = $branch } | ConvertTo-Json
    $null = Invoke-RestMethod -Method Patch -Uri "https://api.github.com/repos/$Owner/$Repository" -Headers $headers -ContentType 'application/json' -Body $defaultBranch -TimeoutSec 15
} catch {
    throw 'Code was pushed, but updating GitHub repository metadata failed.'
}
Write-Output "Published https://github.com/$Owner/$Repository on $branch"
