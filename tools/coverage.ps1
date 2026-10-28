$root = Split-Path $PSScriptRoot
$b = & "$PSScriptRoot/build.ps1" -Preset debug
if ($LASTEXITCODE -ne 0) { $b; Write-Output 'COVERAGE FAIL (build)'; exit 1 }
$ErrorActionPreference = 'Continue'
Set-Location $root
$occ = 'C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe'
$exe = Get-ChildItem "$root/build/debug/tests" -Filter eb_tests.exe | Select-Object -First 1
$dll = Get-ChildItem "$root/build/debug" -Recurse -Filter eb_mupdf.dll | Select-Object -First 1
$outDir = "$root/build/coverage"
New-Item -ItemType Directory -Force $outDir | Out-Null
$xml = "$outDir/cobertura.xml"
Remove-Item $xml -ErrorAction SilentlyContinue
$env:PATH = "$($dll.DirectoryName);$env:PATH"
& $occ --quiet --sources "$root\src" --excluded_sources "$root\src\ui" --export_type "cobertura:$xml" -- $exe.FullName | Out-Null
if (-not (Test-Path $xml)) { Write-Output 'COVERAGE FAIL (no report)'; exit 1 }
$rate = [double](([xml](Get-Content $xml)).coverage.'line-rate')
$pct = [math]::Round($rate * 100, 1)
Write-Output "COVERAGE $pct% (src excl ui)"
if ($pct -lt 80) { exit 1 }
exit 0
