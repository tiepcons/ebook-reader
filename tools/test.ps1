param([ValidateSet('debug', 'release', 'asan', 'lint')][string]$Preset = 'debug')
$root = Split-Path $PSScriptRoot
$b = & "$PSScriptRoot/build.ps1" -Preset $Preset
if ($LASTEXITCODE -ne 0) { $b; Write-Output 'TESTS FAIL'; exit 1 }
. "$PSScriptRoot/env.ps1"
$ErrorActionPreference = 'Continue'
Set-Location $root
$out = & ctest --preset $Preset 2>&1 | ForEach-Object { "$_" }
$code = $LASTEXITCODE
$sum = $out | Where-Object { $_ -match 'tests passed' } | Select-Object -Last 1
$total = if ($sum -match 'out of (\d+)') { [int]$Matches[1] } else { 0 }
$failed = if ($sum -match ', (\d+) tests failed') { [int]$Matches[1] } else { 0 }
if ($code -eq 0) {
    Write-Output ("TESTS OK {0}/{1} {2}" -f ($total - $failed), $total, $Preset)
    exit 0
}
$out | Where-Object { $_ -match '\*\*\*Failed|\(Failed\)|Exception' } | Select-Object -First 10
$out | Where-Object { $_ -match 'FAILED|ERROR|error|Assertion' } | Select-Object -First 10
Write-Output 'TESTS FAIL'
exit 1
