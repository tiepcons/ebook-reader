# Static analysis gate: MSVC /analyze build + cppcheck. Exit 1 on any finding in src/ or libs/.
$root = Split-Path $PSScriptRoot
. "$PSScriptRoot/env.ps1"
$ErrorActionPreference = 'Continue'
Set-Location $root
$fail = $false

$mu = & "$PSScriptRoot/build_mupdf.ps1" -Config Debug
if (-not ($mu | Where-Object { $_ -match '^MUPDF OK' })) { Write-Output 'ANALYZE FAIL mupdf build'; exit 1 }
$bdir = "$root/build/analyze"
$flags = '/analyze /analyze:external- /analyze:quiet'
$out = & cmake --preset debug -B $bdir "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_CXX_FLAGS=$flags" 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { $out | Select-Object -Last 20; Write-Output 'ANALYZE FAIL configure'; exit 1 }
$out = & cmake --build $bdir -- -k 0 2>&1 | ForEach-Object { "$_" }
$hits = @($out | Where-Object { $_ -match '(warning|error) (C\d+|LNK\d+)' -and $_ -notmatch '[\/]third_party[\/]' } | Sort-Object -Unique)
$hits | Select-Object -First 40 | ForEach-Object { Write-Output $_ }
if ($hits.Count -gt 0) { $fail = $true }
Write-Output "MSVC /analyze: $($hits.Count) finding(s)"

$cpp = 'C:\Program Files\Cppcheck\cppcheck.exe'
$cout = & $cpp --enable=warning,performance,portability --inline-suppr --error-exitcode=1 --quiet `
    --std=c++20 --platform=win64 -D_WIN32 -DUNICODE -D_UNICODE -I src -I libs/eb_mupdf -I third_party/mupdf/include `
    --suppress=missingInclude --template='{file}:{line}: {severity}: {message} [{id}]' src libs/eb_mupdf 2>&1 | ForEach-Object { "$_" }
$cc = @($cout | Where-Object { $_ -match ': (error|warning|style|performance|portability):' })
$cc | Select-Object -First 40 | ForEach-Object { Write-Output $_ }
if ($cc.Count -gt 0 -or $LASTEXITCODE -ne 0) { $fail = $true }
Write-Output "cppcheck: $($cc.Count) finding(s)"
if ($fail) { Write-Output 'ANALYZE FAIL'; exit 1 }
Write-Output 'ANALYZE OK'
exit 0
