param([string]$Owner = 'michaelgrenfellbrown')

$ErrorActionPreference = 'Stop'
$env:GCM_INTERACTIVE = 'never'
$env:GIT_TERMINAL_PROMPT = '0'
$request = "protocol=https`nhost=github.com`nusername=$Owner`n`n"
# Keep credential output in memory only; never print it or include it in errors.
$credentialLines = $request | git credential fill 2>$null
if ($LASTEXITCODE -ne 0) {
    throw "No non-interactive GitHub credential was available for $Owner. Sign in before publishing."
}
$credential = @{}
foreach ($line in $credentialLines) {
    $pair = $line -split '=', 2
    if ($pair.Length -eq 2) { $credential[$pair[0]] = $pair[1] }
}
if (-not $credential.password) { throw 'No usable GitHub credential was available.' }
$headers = @{ Authorization = "Bearer $($credential.password)"; Accept = 'application/vnd.github+json'; 'User-Agent' = 'Xtatech-ONT-Watchdog' }
try {
    $githubProfile = Invoke-RestMethod -Uri 'https://api.github.com/user' -Headers $headers -TimeoutSec 15
} catch {
    throw 'GitHub authentication check failed. No repository was created.'
}
if ($githubProfile.login -ne $Owner) { throw "The available credential belongs to a different account, not $Owner." }
Write-Output "Authenticated GitHub account: $($githubProfile.login). This check did not create or publish a repository."
