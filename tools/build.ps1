param(
    [Parameter(Mandatory)][ValidateSet('debug', 'release', 'asan', 'lint')][string]$Preset,
    [string]$Target,
    [string]$VersionSuffix
)
$root = Split-Path $PSScriptRoot
. "$PSScriptRoot/env.ps1"
$ErrorActionPreference = 'Continue'
Set-Location $root
$sw = [Diagnostics.Stopwatch]::StartNew()
$bdir = "$root/build/$Preset"
$out = @()
$mu = & "$PSScriptRoot/build_mupdf.ps1" -Config $(if ($Preset -eq 'release') { 'Release' } else { 'Debug' })
if (-not ($mu | Where-Object { $_ -match '^MUPDF OK' })) { $mu | Select-Object -Last 20; Write-Output "BUILD FAIL $Preset"; exit 1 }
$ok = $true
$cache = Get-Item "$bdir/CMakeCache.txt" -ErrorAction SilentlyContinue
$inputs = @(Get-Item "$root/CMakePresets.json") + @(Get-ChildItem $root -Recurse -Filter CMakeLists.txt |
    Where-Object { $_.FullName -notmatch '[\/](build|third_party)[\/]' })
$stale = (-not $cache) -or ($inputs | Where-Object { $_.LastWriteTime -gt $cache.LastWriteTime })
$cfgArgs = @('--preset', $Preset)
if ($PSBoundParameters.ContainsKey('VersionSuffix')) {
    $cfgArgs += "-DEB_VERSION_SUFFIX=$VersionSuffix"
    $cached = if ($cache) { (Select-String -Path $cache.FullName -Pattern '^EB_VERSION_SUFFIX:STRING=(.*)$').Matches.Groups[1].Value }
    if ($cached -ne $VersionSuffix) { $stale = $true }
}
if ($stale) {
    $out += & cmake @cfgArgs 2>&1 | ForEach-Object { "$_" }
    $ok = ($LASTEXITCODE -eq 0)
}
if ($ok) {
    $buildArgs = @('--build', '--preset', $Preset)
    if ($Target) { $buildArgs += @('--target', $Target) }
    $out += & cmake @buildArgs 2>&1 | ForEach-Object { "$_" }
    $ok = ($LASTEXITCODE -eq 0)
}
if (-not $ok) {
    $seen = @{}
    $hits = foreach ($l in ($out | Where-Object { $_ -match 'error|fatal|warning [A-Z]+\d+|FAILED:' })) {
        $key = if ($l -match '^(.+?)[(:]\d+') { $Matches[1] } else { $l }
        if (-not $seen[$key]) { $seen[$key] = $true; $l }
    }
    if (-not $hits) { $hits = $out }
    $hits | Select-Object -Last 20 | ForEach-Object { Write-Output $_ }
    Write-Output "BUILD FAIL $Preset"
    exit 1
}
$line = "BUILD OK $Preset"
$exe = "$bdir/EbookReader.exe"
if (Test-Path $exe) { $line += " EbookReader.exe=$([math]::Round((Get-Item $exe).Length / 1KB))KB" }
$dll = Get-ChildItem $bdir -Recurse -Filter eb_mupdf.dll -ErrorAction SilentlyContinue | Select-Object -First 1
if ($dll) { $line += " eb_mupdf.dll=$([math]::Round($dll.Length / 1KB))KB" }
Write-Output "$line $([math]::Round($sw.Elapsed.TotalSeconds))s"
exit 0
