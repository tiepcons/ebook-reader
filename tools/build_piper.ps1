# Builds the Piper runtime (piper.exe, espeak-ng, piper-phonemize, onnxruntime) from the commits pinned in
# tools/piper_pins.psd1 into build/piper-src/out. Sources come from git by full commit SHA, the onnxruntime zip is
# verified by SHA256. MSVC /MD like upstream (onnxruntime.dll needs the VC++ runtime anyway, and the DLLs exchange
# std:: types). Only deviation from upstream sources: the newline at the end of piper/VERSION is stripped, because
# Ninja passes it into a /D define. build/piper-src/out.stamp holds the SHA256 of the pins file used for the build.
# espeak-ng's cmake would fetch libsonic (unpinned by us, and unused with USE_LIBSONIC=OFF), so SONIC_* are preset to skip it.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/env.ps1"
$root = Split-Path $PSScriptRoot
$work = Join-Path $root 'build/piper-src'
$pins = Import-PowerShellDataFile "$PSScriptRoot/piper_pins.psd1"
$src = "$work/src"; $bld = "$work/b"; $ins = "$work/i"; $out = "$work/out"; $logs = "$work/logs"; $stamp = "$work/out.stamp"

function Invoke-Step([string]$Name, [string[]]$Cmd) {
    $log = "$logs/$Name.log"
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    & $Cmd[0] $Cmd[1..($Cmd.Count - 1)] *> $log
    $code = $LASTEXITCODE; $ErrorActionPreference = $old
    if ($code -ne 0) { Get-Content $log -Tail 25 | Out-Host; throw "step '$Name' failed (exit $code), log: $log" }
}

function Get-Source([string]$Name) {
    $p = $pins.Sources[$Name]; $dir = "$src/$Name"
    if (-not (Test-Path "$dir/.git")) {
        New-Item -ItemType Directory -Force $dir | Out-Null
        Invoke-Step "fetch-$Name" @('git', '-C', $dir, 'init', '-q')
        Invoke-Step "fetch-$Name" @('git', '-C', $dir, 'remote', 'add', 'origin', $p.Url)
    }
    if ((& git -C $dir rev-parse -q --verify "$($p.Commit)^{commit}" 2>$null) -ne $p.Commit) {
        Invoke-Step "fetch-$Name" @('git', '-C', $dir, 'fetch', '-q', '--depth', '1', 'origin', $p.Commit)
    }
    Invoke-Step "checkout-$Name" @('git', '-C', $dir, 'checkout', '-q', '-f', '--detach', $p.Commit)
    Invoke-Step "clean-$Name" @('git', '-C', $dir, 'clean', '-ffdxq')
    if ((& git -C $dir rev-parse HEAD) -ne $p.Commit) { throw "$Name is not at pinned commit $($p.Commit)" }
}

function Get-Onnxruntime {
    $o = $pins.Onnxruntime
    $zip = Join-Path $root "build/downloads/$($o.Sha256)-$(Split-Path $o.Url -Leaf)"
    if (-not (Test-Path $zip) -or (Get-FileHash $zip -Algorithm SHA256).Hash -ne $o.Sha256.ToUpper()) {
        New-Item -ItemType Directory -Force (Split-Path $zip) | Out-Null
        Invoke-WebRequest -Uri $o.Url -OutFile "$zip.part" -UseBasicParsing
        if ((Get-FileHash "$zip.part" -Algorithm SHA256).Hash -ne $o.Sha256.ToUpper()) { Remove-Item "$zip.part"; throw "SHA256 mismatch: $($o.Url)" }
        Move-Item "$zip.part" $zip -Force
    }
    Expand-Archive $zip -DestinationPath "$work/ort" -Force
    "$work/ort/$($o.Dir)"
}

function Build-Cmake([string]$Name, [string]$Install, [string[]]$Extra, [string]$Target = '') {
    $cfg = @('cmake', '-S', "$src/$Name", '-B', "$bld/$Name", '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_INSTALL_PREFIX=$Install") + $Extra
    Invoke-Step "configure-$Name" $cfg
    $build = @('cmake', '--build', "$bld/$Name")
    if ($Target) { $build += @('--target', $Target) }
    Invoke-Step "build-$Name" $build
    Invoke-Step "install-$Name" @('cmake', '--install', "$bld/$Name")
}

foreach ($d in $bld, $ins, $out, $logs, "$work/ort", $stamp) { if (Test-Path $d) { Remove-Item $d -Recurse -Force } }
New-Item -ItemType Directory -Force $logs | Out-Null
foreach ($n in $pins.Sources.Keys) { Get-Source $n }
$ort = Get-Onnxruntime
$verFile = "$src/piper/VERSION"
[IO.File]::WriteAllText($verFile, (Get-Content $verFile -Raw).Trim())

Build-Cmake 'espeak' "$ins/espeak" @('-DUSE_ASYNC=OFF', '-DBUILD_SHARED_LIBS=ON', '-DUSE_MBROLA=OFF', '-DUSE_LIBSONIC=OFF',
    '-DUSE_LIBPCAUDIO=OFF', '-DUSE_KLATT=OFF', '-DUSE_SPEECHPLAYER=OFF', '-DEXTRA_cmn=ON', '-DEXTRA_ru=ON',
    '-DCMAKE_C_FLAGS=-D_FILE_OFFSET_BITS=64', '-DSONIC_LIB=unused', '-DSONIC_INC=unused')
Build-Cmake 'phonemize' "$ins/pp" @("-DESPEAK_NG_DIR=$ins/espeak", "-DONNXRUNTIME_DIR=$ort")
Build-Cmake 'fmt' "$ins/fmt" @('-DFMT_TEST=OFF', '-DFMT_DOC=OFF')
Build-Cmake 'spdlog' "$ins/spdlog" @()
Build-Cmake 'piper' $out @("-DFMT_DIR=$ins/fmt", "-DSPDLOG_DIR=$ins/spdlog", "-DPIPER_PHONEMIZE_DIR=$ins/pp") 'piper'
if (Test-Path "$out/pkgconfig") { Remove-Item "$out/pkgconfig" -Recurse -Force }
if (-not (Test-Path "$out/piper.exe") -or -not (Test-Path "$out/espeak-ng-data")) { throw 'build output incomplete' }
Set-Content $stamp (Get-FileHash "$PSScriptRoot/piper_pins.psd1" -Algorithm SHA256).Hash
'PIPER OK {0}' -f $out
