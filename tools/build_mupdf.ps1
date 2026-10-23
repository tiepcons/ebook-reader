param([Parameter(Mandatory)][ValidateSet('Release', 'Debug')][string]$Config)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\env.ps1"
$root = Split-Path $PSScriptRoot
$src = Join-Path $root 'third_party\mupdf'
$out = Join-Path $root "build\mupdf\$Config"
$props = Join-Path $PSScriptRoot 'mupdf.props'
$libs = 'libthirdparty', 'libresources', 'libharfbuzz', 'libextract', 'libpkcs7', 'libmupdf'
$steps = @(, @("bin2coff", "Release", "Win32")) + ($libs | ForEach-Object { , @($_, $(if ($_ -eq "libresources") { "Release" } else { $Config }), "x64") })

function Get-Libs { $libs | ForEach-Object { Get-Item "$out\$_.lib" -ErrorAction SilentlyContinue } }

$head = @([datetime]::Parse((git -C $src log -1 --format=%cI HEAD)), (Get-Item $props).LastWriteTime,
    (Get-Item $PSCommandPath).LastWriteTime) | Sort-Object | Select-Object -Last 1
$have = @(Get-Libs)
$upToDate = $have.Count -eq $libs.Count -and -not ($have | Where-Object { $_.LastWriteTime -le $head })
if (-not $upToDate) {
    New-Item -ItemType Directory -Force $out | Out-Null
    $log = Join-Path $out 'msbuild.log'
    Remove-Item $log -ErrorAction SilentlyContinue
    foreach ($s in $steps) {
        msbuild "$src\platform\win32\$($s[0]).vcxproj" "/p:Configuration=$($s[1])" "/p:EbCfg=$Config" "/p:Platform=$($s[2])" `
            /p:PlatformToolset=v143 "/p:ForceImportBeforeCppTargets=$props" `
            "/p:EbMuRoot=$root\build\mupdf" /m /nologo /v:q /clp:ErrorsOnly "/flp:logfile=$log;errorsonly;append" | Out-Null
        if ($LASTEXITCODE -ne 0) { break }
    }
    $have = @(Get-Libs)
    if ($LASTEXITCODE -ne 0 -or $have.Count -ne $libs.Count) {
        Get-Content $log -ErrorAction SilentlyContinue | Select-Object -First 20
        'MUPDF FAIL'
        exit 1
    }
}
$list = ($have | ForEach-Object { '{0} {1}KB' -f $_.Name, [int]($_.Length / 1KB) }) -join ' '
"MUPDF OK $Config $list"
