# Local release: CI -> package -> sign -> SHA256SUMS. No git tag, no upload.
param(
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$')][string]$Version,
    [switch]$SkipCi
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
if (-not $SkipCi) {
    & "$PSScriptRoot/ci.ps1"
    if ($LASTEXITCODE -ne 0) { throw 'CI failed' }
}
& "$PSScriptRoot/package.ps1" -Version $Version -Sign
$dist = Join-Path $root 'dist'
$sums = Join-Path $dist 'SHA256SUMS.txt'
Get-ChildItem $dist -Filter "EbookReader-$Version-*.zip" |
    ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name } |
    Set-Content $sums -Encoding ascii
Get-Content $sums
