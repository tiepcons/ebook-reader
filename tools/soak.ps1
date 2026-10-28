param(
    [Parameter(Mandatory)][ValidateSet('release', 'asan')][string]$Preset,
    [Parameter(Mandatory)][ValidateRange(1, 10080)][int]$Minutes
)
$root = Split-Path $PSScriptRoot
$b = & "$PSScriptRoot/build.ps1" -Preset $Preset
if ($LASTEXITCODE -ne 0) { $b; Write-Output 'SOAK FAIL build'; exit 1 }
$bdir = "$root/build/$Preset"
$dll = Get-ChildItem $bdir -Recurse -Filter eb_mupdf.dll | Select-Object -First 1
$dir = "$bdir/soak"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force "$dir/libs", "$dir/ebooks" | Out-Null
Copy-Item "$bdir/EbookReader.exe" "$dir/EbookReader.exe"
Copy-Item $dll.FullName "$dir/libs/eb_mupdf.dll"
Copy-Item "$root/tests/corpus/good/*" "$dir/ebooks" -Recurse
$exe = "$dir/EbookReader.exe"
$scan = Start-Process $exe -ArgumentList '--smoke', '--smoke-scan' -Wait -PassThru
if ($scan.ExitCode -ne 0) { Write-Output "SOAK FAIL scan exit=$($scan.ExitCode)"; exit 1 }

$err = "$dir/stderr.txt"
$p = Start-Process $exe -ArgumentList '--smoke', '--smoke-soak', $Minutes -PassThru -RedirectStandardError $err
$null = $p.Handle
if (-not $p.WaitForExit(($Minutes + 5) * 60000)) { Write-Output 'SOAK FAIL timeout (process left running)'; exit 1 }
$code = $p.ExitCode
$asan = (Test-Path $err) -and (Select-String -Path $err -Pattern 'AddressSanitizer' -Quiet)
$rows = @(Get-Content "$dir/data/logs/app.log" | ForEach-Object {
    if ($_ -match 'soak: t=(\d+) books=(\d+) pages=(\d+) ws=(\d+) private=(\d+) handles=(\d+) gdi=(\d+)') {
        [pscustomobject]@{ t = [int]$Matches[1]; books = [int]$Matches[2]; pages = [int]$Matches[3]; ws = [long]$Matches[4]
            priv = [long]$Matches[5]; handles = [long]$Matches[6]; gdi = [long]$Matches[7] }
    }
})
if ($rows.Count -lt 2) { Write-Output "SOAK FAIL no soak samples exit=$code"; exit 1 }
$first = $rows[0]; $last = $rows[-1]
$warm = $rows | Where-Object { $_.t -ge [math]::Max(5, [math]::Ceiling($Minutes * 0.1)) } | Select-Object -First 1
$why = @()
if ($code -ne 0) { $why += "exit=$code" }
if ($asan) { $why += 'asan-report' }
if ($last.books -lt 1) { $why += 'no-book-opened' }
foreach ($m in 'priv', 'handles', 'gdi') {
    # warm-up is the first max(10%, 5 min); shorter runs only check exit code and ASan
    if ($warm -and $warm.$m -gt 0 -and $last.$m -gt $warm.$m * 1.1) { $why += "$m-grew($($warm.$m)->$($last.$m))" }
}
$line = "minutes=$Minutes books=$($last.books) pages=$($last.pages) ws_start=$($first.ws) ws_end=$($last.ws) handles_start=$($first.handles) handles_end=$($last.handles)"
if ($why) { Write-Output "SOAK FAIL $line $($why -join ' ')"; exit 1 }
Write-Output "SOAK OK $line"
exit 0
