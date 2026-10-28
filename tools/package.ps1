# Stages dist/EbookReader-<ver>/ and writes the basic zip and the Piper pack zip. Downloads are cached in
# build/downloads and verified against pinned SHA256; they are only extracted, never executed.
param(
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$')][string]$Version,
    [switch]$Sign
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$root = Split-Path $PSScriptRoot
$dist = Join-Path $root 'dist'
$dl = Join-Path $root 'build/downloads'
$name = "EbookReader-$Version"
$stage = Join-Path $dist $name
$maxBasicBytes = 10MB

$hf = 'https://huggingface.co/rhasspy/piper-voices/resolve/main'
$gh = 'https://github.com/rhasspy/piper/releases/download/2023.11.14-2'
$pins = @{
    piper = @("$gh/piper_windows_amd64.zip", 'f3c58906402b24f3a96d92145f58acba6d86c9b5db896d207f78dc80811efcea')
    piperLic = @('https://raw.githubusercontent.com/rhasspy/piper/2023.11.14-2/LICENSE.md', '4cd71dece7037f1d6d93cce7570c57ab75ea9ac566fd4990be2f3ab08d15b47f')
    espeakLic = @('https://raw.githubusercontent.com/espeak-ng/espeak-ng/1.52.0/COPYING', '8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903')
    ortLic = @('https://raw.githubusercontent.com/microsoft/onnxruntime/v1.14.1/LICENSE', '2f07c72751aed99790b8a4869cf2311df85a860b22ded05fa22803587a48922c')
    viOnnx = @("$hf/vi/vi_VN/vais1000/medium/vi_VN-vais1000-medium.onnx", 'ec7c89e2c85f4d1edc24b6120c18aaf1bda614f06b511567eb9c7c0de15e2dab')
    viJson = @("$hf/vi/vi_VN/vais1000/medium/vi_VN-vais1000-medium.onnx.json", 'fafb9da1354ed4b77c31af228ed41fb41cd825c14cffa105454b25e6ae751ee0')
    viCard = @("$hf/vi/vi_VN/vais1000/medium/MODEL_CARD", '302db8a930ffc2b1c2181db26deaf8272116eca2b08b8f80da6375b0a994af7b')
    enOnnx = @("$hf/en/en_US/lessac/medium/en_US-lessac-medium.onnx", '5efe09e69902187827af646e1a6e9d269dee769f9877d17b16b1b46eeaaf019f')
    enJson = @("$hf/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json", 'efe19c417bed055f2d69908248c6ba650fa135bc868b0e6abb3da181dab690a0')
    enCard = @("$hf/en/en_US/lessac/medium/MODEL_CARD", 'ce49eb457742208166d399a40cdec2c7fa9db77960930031564ab56f12882645')
}

function Get-Pinned([string]$Key) {
    $url, $sha = $pins[$Key]
    $file = Join-Path $dl "$sha-$([IO.Path]::GetFileName(([Uri]$url).AbsolutePath))"
    if (-not (Test-Path $file) -or (Get-FileHash $file -Algorithm SHA256).Hash -ne $sha.ToUpper()) {
        New-Item -ItemType Directory -Force $dl | Out-Null
        $tmp = "$file.part"
        Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing
        if ((Get-FileHash $tmp -Algorithm SHA256).Hash -ne $sha.ToUpper()) { Remove-Item $tmp; throw "SHA256 mismatch: $url" }
        Move-Item $tmp $file -Force
    }
    $file
}

function Expand-Safe([string]$Zip, [string]$Dest) {
    $full = [IO.Path]::GetFullPath($Dest).TrimEnd('\') + '\'
    $za = [IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        foreach ($e in $za.Entries) {
            $target = [IO.Path]::GetFullPath((Join-Path $Dest $e.FullName))
            if (-not $target.StartsWith($full, [StringComparison]::OrdinalIgnoreCase)) { throw "unsafe zip entry: $($e.FullName)" }
            if ($e.FullName.EndsWith('/')) { New-Item -ItemType Directory -Force $target | Out-Null; continue }
            New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $target, $false)
        }
    } finally { $za.Dispose() }
}

function New-Zip([string]$Zip, [string]$Base, [string[]]$Files, [string[]]$EmptyDirs = @()) {
    if (Test-Path $Zip) { Remove-Item $Zip }
    $za = [IO.Compression.ZipFile]::Open($Zip, 'Create')
    try {
        foreach ($f in $Files) {
            $rel = $f.Substring($Base.Length).TrimStart('\').Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f, "$name/$rel", 'Optimal')
        }
        foreach ($d in $EmptyDirs) { [void]$za.CreateEntry("$name/$d/") }
    } finally { $za.Dispose() }
}

function Copy-To([string]$Src, [string]$Dst) {
    New-Item -ItemType Directory -Force (Split-Path $Dst) | Out-Null
    Copy-Item -LiteralPath $Src -Destination $Dst
}

function Write-BundledLicenses([string]$Lic) {
    $mu = "$root/third_party/mupdf/thirdparty"
    $items = @(@('freetype/docs/FTL.TXT', 'FreeType-FTL'), @('harfbuzz/COPYING', 'HarfBuzz'), @('gumbo-parser/COPYING', 'gumbo'),
        @('jbig2dec/LICENSE', 'jbig2dec'), @('lcms2/LICENSE', 'lcms2'), @('openjpeg/LICENSE', 'OpenJPEG'), @('zlib/LICENSE', 'zlib'),
        @('brotli/LICENSE', 'brotli'), @('mujs/COPYING', 'MuJS'), @('extract/COPYING', 'extract'))
    foreach ($m in $items) { Copy-To "$mu/$($m[0])" "$Lic/mupdf-bundled/LICENSE-$($m[1]).txt" }
    $sq = (Get-Content "$root/third_party/sqlite/sqlite3.h")[2..8] -join "`r`n"
    Set-Content "$Lic/LICENSE-SQLite-public-domain.txt" "SQLite 3.54.1 (third_party/sqlite/sqlite3.h) is in the public domain. Its notice reads:`r`n`r`n$sq" -Encoding utf8
    $mz = Get-Content "$root/third_party/libmobi/src/miniz.c"
    $start = ($mz | Select-String -SimpleMatch 'This is free and unencumbered software').LineNumber
    $body = $mz[($start - 1)..($mz.Count - 1)] | Where-Object { $_ -notmatch '^\s*\*/\s*$' }
    Set-Content "$Lic/LICENSE-miniz-unlicense.txt" "miniz v1.15 (inside libmobi). Notice from third_party/libmobi/src/miniz.c:`r`n`r`n$($body -join "`r`n")" -Encoding utf8
}

function Write-BasicDocs([string]$Stage) {
    $lic = "$Stage/licenses"
    Copy-To "$root/LICENSE" "$lic/LICENSE-EbookReader-AGPL-3.0.txt"
    Copy-To "$root/third_party/mupdf/COPYING" "$lic/LICENSE-MuPDF-AGPL-3.0.txt"
    Copy-To "$root/third_party/libmobi/COPYING" "$lic/LICENSE-libmobi-LGPL-3.0.txt"
    Copy-To "$root/fonts/Literata/OFL.txt" "$lic/OFL-Literata.txt"
    Copy-To "$root/fonts/BeVietnamPro/OFL.txt" "$lic/OFL-BeVietnamPro.txt"
    Write-BundledLicenses $lic
    @'
THIRD-PARTY NOTICES - Ebook Reader (basic package)
===================================================
Ebook Reader is free software under the GNU AGPL-3.0. Corresponding source code (this exact version, including the
build scripts needed to rebuild and relink every component below) is available at:
    https://github.com/tiepcons/ebook-reader

Component             Version       License                  Source
--------------------  ------------  -----------------------  ----------------------------------------------
Ebook Reader          see README    AGPL-3.0-or-later        https://github.com/tiepcons/ebook-reader
MuPDF                 1.28.5        AGPL-3.0                 https://mupdf.com , https://github.com/ArtifexSoftware/mupdf
  bundled in MuPDF: FreeType (FTL), HarfBuzz, gumbo, jbig2dec, lcms2, OpenJPEG, zlib, brotli, MuJS, extract
  (license texts in licenses\mupdf-bundled\)
libmobi               0.12          LGPL-3.0-or-later        https://github.com/bfabiszewski/libmobi
miniz (inside libmobi) 1.15         Unlicense / public domain https://github.com/richgel999/miniz
SQLite                3.54.1        Public domain            https://sqlite.org
Literata (font)       google/fonts  SIL OFL 1.1              https://github.com/google/fonts/tree/main/ofl/literata
Be Vietnam Pro (font) google/fonts  SIL OFL 1.1              https://github.com/google/fonts/tree/main/ofl/bevietnampro

LGPL-3.0 note (libmobi): libmobi is compiled statically into EbookReader.exe. You may modify libmobi and relink:
third_party/libmobi in the repository above is the unmodified upstream tree at the version listed, and the repository
contains the build scripts that produce EbookReader.exe. MuPDF is shipped as the separate libs\eb_mupdf.dll, built
from third_party/mupdf with tools/build_mupdf.ps1.
'@ | Set-Content "$lic/THIRD-PARTY-NOTICES.txt" -Encoding utf8
    @"
EBOOK READER $Version

[Tieng Viet]
- Chay EbookReader.exe (khong can cai dat).
- Bo sach (PDF, EPUB, MOBI, ...) vao thu muc ebooks\ canh EbookReader.exe.
- Doc to offline bang Piper (tuy chon): giai nen goi $name-piper-pack.zip len cung thu muc.
  Giong noi (tieng Viet + tieng Anh) nam o data\voices\.
- Ban alpha ky bang chung chi tu ky, nen Windows SmartScreen co the canh bao.
- Giay phep: thu muc licenses\. Ma nguon: https://github.com/tiepcons/ebook-reader

[English]
- Run EbookReader.exe (portable, no installer).
- Put your books (PDF, EPUB, MOBI, ...) in the ebooks\ folder next to EbookReader.exe.
- Optional offline read-aloud with Piper: extract $name-piper-pack.zip into the same folder.
  Voices (Vietnamese + English) go to data\voices\.
- This alpha is signed with a self-signed certificate, so Windows SmartScreen may warn.
- Licenses: licenses\ folder. Source code: https://github.com/tiepcons/ebook-reader
"@ | Set-Content "$Stage/README.txt" -Encoding utf8
}

function Build-Basic {
    $rel = Join-Path $root 'build/release'
    $exe = Join-Path $rel 'EbookReader.exe'
    $dll = Join-Path $rel 'libs/eb_mupdf.dll'
    foreach ($f in $exe, $dll) { if (-not (Test-Path $f)) { throw "missing $f" } }
    New-Item -ItemType Directory -Force $stage, "$stage/ebooks" | Out-Null
    Copy-To $exe "$stage/EbookReader.exe"
    Copy-To $dll "$stage/libs/eb_mupdf.dll"
    if ($Sign) {
        & "$PSScriptRoot/sign.ps1" -Path "$stage/EbookReader.exe", "$stage/libs/eb_mupdf.dll" | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'signing failed' }
    }
    foreach ($f in 'Literata/Literata[opsz,wght].ttf', 'Literata/Literata-Italic[opsz,wght].ttf',
        'BeVietnamPro/BeVietnamPro-Regular.ttf', 'BeVietnamPro/BeVietnamPro-Bold.ttf') {
        Copy-To (Join-Path $root "fonts/$f") "$stage/fonts/$f"
    }
    Write-BasicDocs $stage
    $zip = Join-Path $dist "$name-win-x64.zip"
    New-Zip $zip $stage @(Get-ChildItem $stage -Recurse -File | ForEach-Object FullName) @('ebooks')
    if ((Get-Item $zip).Length -gt $maxBasicBytes) { throw 'basic zip exceeds 10 MB' }
    $zip
}

function Build-PiperPack {
    $pk = Join-Path $dist "$name-piper-pack-stage"
    $ex = Join-Path $pk 'extract'
    New-Item -ItemType Directory $pk, $ex, "$pk/libs", "$pk/data/voices", "$pk/licenses/piper-pack" | Out-Null
    Expand-Safe (Get-Pinned 'piper') $ex
    if (-not (Test-Path "$ex/piper/piper.exe")) { throw 'piper.exe missing in archive' }
    Move-Item "$ex/piper" "$pk/libs/piper"
    Remove-Item $ex -Recurse
    foreach ($k in 'viOnnx', 'viJson', 'enOnnx', 'enJson') {
        Copy-Item (Get-Pinned $k) "$pk/data/voices/$([IO.Path]::GetFileName(([Uri]$pins[$k][0]).AbsolutePath))"
    }
    $pl = "$pk/licenses/piper-pack"
    Copy-Item (Get-Pinned 'piperLic') "$pl/LICENSE-piper-MIT.txt"
    Copy-Item (Get-Pinned 'espeakLic') "$pl/LICENSE-espeak-ng-GPL-3.0.txt"
    Copy-Item (Get-Pinned 'ortLic') "$pl/LICENSE-onnxruntime-MIT.txt"
    Copy-Item (Get-Pinned 'viCard') "$pl/MODEL_CARD-vi_VN-vais1000-medium.md"
    Copy-Item (Get-Pinned 'enCard') "$pl/MODEL_CARD-en_US-lessac-medium.md"
    @'
THIRD-PARTY NOTICES - Piper pack (optional; unmodified downloads from the URLs below)
======================================================================================
Component                    Version        License     Source
---------------------------  -------------  ----------  ------------------------------------------------------
piper                        2023.11.14-2   MIT         https://github.com/rhasspy/piper/releases/tag/2023.11.14-2
espeak-ng (inside piper)     1.52           GPL-3.0     https://github.com/espeak-ng/espeak-ng (source offer, tag 1.52.0)
piper-phonemize (in piper)   as released    MIT         https://github.com/rhasspy/piper-phonemize
onnxruntime (inside piper)   1.14.1         MIT         https://github.com/microsoft/onnxruntime
voice vi_VN-vais1000-medium  -              CC BY 4.0 (VAIS-1000 corpus, see MODEL_CARD)
    https://huggingface.co/rhasspy/piper-voices/tree/main/vi/vi_VN/vais1000/medium
voice en_US-lessac-medium    -              Blizzard 2013 Lessac license (see link in MODEL_CARD)
    https://huggingface.co/rhasspy/piper-voices/tree/main/en/en_US/lessac/medium

Piper runs as a separate process (libs\piper\piper.exe); Ebook Reader only starts it and exchanges text and audio.
espeak-ng is GPL-3.0: its source is available at the URL above. Ebook Reader's own source:
https://github.com/tiepcons/ebook-reader
'@ | Set-Content "$pl/THIRD-PARTY-NOTICES-piper-pack.txt" -Encoding utf8
    $zip = Join-Path $dist "$name-piper-pack.zip"
    New-Zip $zip $pk @(Get-ChildItem $pk -Recurse -File | ForEach-Object FullName)
    Remove-Item $pk -Recurse -Force
    $zip
}

$b = & "$PSScriptRoot/build.ps1" -Preset release
if ($LASTEXITCODE -ne 0) { $b; throw 'release build failed' }
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
$zips = @((Build-Basic), (Build-PiperPack))
foreach ($z in $zips) { 'ARTIFACT {0} {1:N2} MB' -f (Split-Path $z -Leaf), ((Get-Item $z).Length / 1MB) }
