param(
    [Parameter(Mandatory)][ValidateSet('mupdf', 'sfnt', 'txt', 'tts', 'azure', 'piper')][string]$Target,
    [int]$Seconds = 60
)
$root = Split-Path $PSScriptRoot
. "$PSScriptRoot/env.ps1"
$ErrorActionPreference = 'Continue'
Set-Location $root
$bdir = "$root/build/fuzz"
$name = "fuzz_$Target"
if (-not (Test-Path "$bdir/CMakeCache.txt") -or (Get-Item "$root/CMakePresets.json").LastWriteTime -gt (Get-Item "$bdir/CMakeCache.txt").LastWriteTime) {
    & cmake --preset fuzz 2>&1 | Out-Null
}
$b = & cmake --build --preset fuzz --target $name 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { $b | Where-Object { $_ -match 'error|LNK' } | Select-Object -First 10; Write-Output "FUZZ BUILD FAIL $Target"; exit 1 }

$corpus = "$bdir/corpus/$Target"
$crashes = "$bdir/artifacts/$Target"
New-Item -ItemType Directory -Force $corpus, $crashes | Out-Null
Get-ChildItem $crashes -File | Remove-Item
$good = "$root/tests/corpus/good"
$bad = "$root/tests/corpus/bad"
$seedCap = 32KB
function Add-Seed([string]$file, [byte[]]$prefix = @(), [int]$cap = $seedCap) {
    $n = [Math]::Min((Get-Item $file).Length, $cap)
    $fs = [IO.File]::OpenRead($file)
    try { $buf = New-Object byte[] $n; [void]$fs.Read($buf, 0, $n) } finally { $fs.Dispose() }
    $hash = (Get-FileHash -InputStream ([IO.MemoryStream]::new($buf)) -Algorithm SHA1).Hash.Substring(0, 16)
    [IO.File]::WriteAllBytes("$corpus/$hash", [byte[]]($prefix + $buf))
}
switch ($Target) {
    'mupdf' {
        $maxLen = 3MB
        $kinds = 'epub', 'fb2', 'cbz', 'pdf', 'mobi'
        foreach ($dir in $good, $bad) {
            Get-ChildItem $dir -Recurse -File | Where-Object { $_.Length -le $maxLen } | ForEach-Object {
                $ext = $_.Extension.TrimStart('.')
                if ($ext -eq 'azw3') { $ext = 'mobi' }
                $i = [Array]::IndexOf($kinds, $ext)
                if ($i -ge 0) { Add-Seed $_.FullName ([byte[]]@($i)) $maxLen }
            }
        }
    }
    'sfnt' {
        $maxLen = 1MB
        Get-ChildItem "$root/fonts" -Recurse -File -Include *.ttf, *.otf, *.ttc | ForEach-Object { Add-Seed $_.FullName @() $maxLen }
    }
    { $_ -in 'txt', 'tts' } {
        $maxLen = 64KB
        Get-ChildItem $good/txt, $good/fb2 -File | ForEach-Object { Add-Seed $_.FullName }
    }
    'azure' {
        $maxLen = 64KB
        '[{"ShortName":"vi-VN-HoaiMyNeural","LocalName":"Hoai My","Locale":"vi-VN"},{"ShortName":"en-US-JennyNeural","LocalName":"Jenny","Locale":"en-US"}]' |
            Set-Content "$corpus/voices" -Encoding utf8
        "vi-VN-HoaiMyNeural`nHello <world> & `"friends`"" | Set-Content "$corpus/ssml" -Encoding utf8
    }
    'piper' {
        $maxLen = 64KB
        '{"audio":{"sample_rate":22050},"espeak":{"voice":"vi"},"language":{"code":"vi_VN"}}' | Set-Content "$corpus/model" -Encoding utf8
    }
}

$log = "$bdir/$Target.log"
$fuzzArgs = @($corpus, "-max_len=$maxLen", "-timeout=30", "-rss_limit_mb=3072", "-max_total_time=$Seconds", '-print_final_stats=1',
    "-artifact_prefix=$crashes/", '-close_fd_mask=3')
$env:ASAN_OPTIONS = 'detect_leaks=0:detect_odr_violation=0:allocator_may_return_null=1:abort_on_error=0'
$sw = [Diagnostics.Stopwatch]::StartNew()
& "$bdir/fuzz/$name.exe" @fuzzArgs 2>&1 | ForEach-Object { "$_" } | Set-Content $log -Encoding utf8
$code = $LASTEXITCODE
$text = Get-Content $log -Raw
$runs = if ($text -match 'stat::number_of_executed_units:\s+(\d+)') { [int64]$Matches[1] } else { 0 }
$eps = if ($text -match 'stat::average_exec_per_sec:\s+(\d+)') { [int64]$Matches[1] } else { 0 }
$cov = ([regex]::Matches($text, 'cov: (\d+)') | Select-Object -Last 1)
$covN = if ($cov) { $cov.Groups[1].Value } else { '?' }
$found = @(Get-ChildItem $crashes -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(crash|leak|timeout|oom|slow-unit)-' })
$status = if ($code -eq 0 -and -not $found) { 'OK' } else { 'FOUND' }
$what = ($text -split "`n" | Where-Object { $_ -match 'SUMMARY:|ERROR: libFuzzer' } | Select-Object -First 1)
Write-Output ("FUZZ {0} {1} runs={2} exec/s={3} cov={4} time={5}s artifacts={6} {7}" -f $status, $Target, $runs, $eps, $covN, [int]$sw.Elapsed.TotalSeconds, $found.Count, $what)
if ($status -ne 'OK') { exit 1 }
