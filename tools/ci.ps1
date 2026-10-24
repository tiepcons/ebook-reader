# Local CI (PLAN.md T0.9): all presets + size budgets. Must be green before pushing a finished phase.
param([switch]$Quick) # -Quick: debug tests + lint only
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot
# PLAN.md §8 allows exe ≤ 3 MB; 2 MB is a tighter regression guard (C++/WinRT TTS + reader push past 1.5 MB).
$budgetKB = @{ 'EbookReader.exe' = 2048; 'eb_mupdf.dll' = 10240 }
$steps = if ($Quick) { @(@('test', 'debug'), @('build', 'lint')) } else {
    @(@('test', 'debug'), @('test', 'asan'), @('build', 'lint'), @('test', 'release')) }
$fail = 0
$t0 = Get-Date
foreach ($s in $steps) {
    $out = & "$PSScriptRoot/$($s[0]).ps1" -Preset $s[1]
    if ($LASTEXITCODE -ne 0) { $fail++; $out | Select-Object -Last 25 } else { $out | Select-Object -Last 1 }
}
if (-not $Quick) {
    foreach ($name in $budgetKB.Keys) {
        $f = Get-ChildItem (Join-Path $root 'build/release') -Recurse -Filter $name -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if (-not $f) { Write-Output "SIZE MISSING $name"; $fail++; continue }
        $kb = [math]::Round($f.Length / 1KB)
        if ($kb -gt $budgetKB[$name]) { Write-Output "SIZE FAIL $name=${kb}KB > $($budgetKB[$name])KB"; $fail++ }
        else { Write-Output "SIZE OK $name=${kb}KB (budget $($budgetKB[$name])KB)" }
    }
}
$secs = [int]((Get-Date) - $t0).TotalSeconds
if ($fail -eq 0) { Write-Output "CI OK ${secs}s"; exit 0 }
Write-Output "CI FAIL ($fail step(s)) ${secs}s"
exit 1
