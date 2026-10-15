param(
    [string]$Owner = 'michaelgrenfellbrown',
    [string]$Repository = 'ONT-Watchdog'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Check-GitHubAccess.ps1') -Owner $Owner
$response = Invoke-RestMethod -Uri "https://api.github.com/repos/$Owner/$Repository/actions/runs?per_page=1" -Headers $headers -TimeoutSec 15
if (-not $response.workflow_runs) { Write-Output 'No workflow run available yet.'; exit 0 }
$run = $response.workflow_runs[0]
Write-Output "Run $($run.id): $($run.status) / $($run.conclusion), revision $($run.head_sha)"
Write-Output $run.html_url
$jobs = Invoke-RestMethod -Uri "https://api.github.com/repos/$Owner/$Repository/actions/runs/$($run.id)/jobs" -Headers $headers -TimeoutSec 15
foreach ($job in $jobs.jobs) {
    Write-Output "$($job.name): $($job.status) / $($job.conclusion)"
    if ($job.conclusion -eq 'failure') {
        $check = Invoke-RestMethod -Uri $job.check_run_url -Headers $headers -TimeoutSec 15
        $annotations = Invoke-RestMethod -Uri "$($job.check_run_url)/annotations" -Headers $headers -TimeoutSec 15
        Write-Output $check.output.summary
        foreach ($annotation in $annotations) { Write-Output "$($annotation.path):$($annotation.start_line) $($annotation.message)" }
    }
}
