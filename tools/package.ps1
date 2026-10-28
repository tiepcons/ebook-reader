# Stages dist/EbookReader-<ver>/ and writes the basic zip and the Piper pack zip. Downloads are cached in
# build/downloads and verified against pinned SHA256; they are only extracted, never executed.
param(
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$')][string]$Version,
    [switch]$NoSign
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$root = Split-Path $PSScriptRoot
$dist = Join-Path $root 'dist'
$dl = Join-Path $root 'build/downloads'
$name = "EbookReader-$Version"
$stage = Join-Path $dist $name
$maxBasicBytes = 10MB

$gh = 'https://github.com/rhasspy/piper/releases/download/2023.11.14-2'
$pins = @{
    piper = @("$gh/piper_windows_amd64.zip", 'f3c58906402b24f3a96d92145f58acba6d86c9b5db896d207f78dc80811efcea')
    piperLic = @('https://raw.githubusercontent.com/rhasspy/piper/2023.11.14-2/LICENSE.md', '4cd71dece7037f1d6d93cce7570c57ab75ea9ac566fd4990be2f3ab08d15b47f')
    phonemizeLic = @('https://raw.githubusercontent.com/rhasspy/piper-phonemize/fccd4f335aa68ac0b72600822f34d84363daa2bf/LICENSE.md', '13746d509d74e55ea2265fbef204bb7cdbf84a8315b0207e988326cb54387028')
    tashkeelLic = @('https://raw.githubusercontent.com/mush42/libtashkeel/9b91c2dbc3dc4085dc4f77a9e250e5dd095d7d0c/LICENSE', '98dbc9a6a01badb391f2b64161742cb9cd8fcdc5cfc97486be0d49c47e98d1a8')
    espeakLic = @('https://raw.githubusercontent.com/rhasspy/espeak-ng/0f65aa301e0d6bae5e172cc74197d32a6182200f/COPYING', '8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903')
    ortLic = @('https://raw.githubusercontent.com/microsoft/onnxruntime/v1.14.1/LICENSE', '2f07c72751aed99790b8a4869cf2311df85a860b22ded05fa22803587a48922c')
    ortNotices = @('https://raw.githubusercontent.com/microsoft/onnxruntime/v1.14.1/ThirdPartyNotices.txt', 'cc63920441c5681cfb1663caf1de1858349b517fc1bed9ca669a85bc526cd13e')
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

function Write-Readme([string]$Stage) {
    $hf = 'https://huggingface.co/rhasspy/piper-voices/resolve/5041a7a49ae5b3a0f30e6837769eb03fdbb231bd'
    @"
EBOOK READER $Version

[Tieng Viet]
- Chay EbookReader.exe (khong can cai dat).
- Bo sach (PDF, EPUB, MOBI, ...) vao thu muc ebooks\ canh EbookReader.exe.
- Doc to offline bang Piper (tuy chon): giai nen goi $name-piper-pack.zip len cung thu muc,
  roi tu tai giong noi (.onnx va .onnx.json) vao data\voices\ (xem muc VOICES ben duoi). Goi nay KHONG kem giong noi.
- Ban alpha: neu file khong co chu ky hop le, Windows SmartScreen co the canh bao.
- Go bo: xoa thu muc ung dung; xoa du lieu o data\ canh EbookReader.exe (hoac %LOCALAPPDATA%\EbookReader neu thu muc
  ung dung khong ghi duoc); neu da dung Azure Speech: cmdkey /delete:EbookReader.tiepcons/AzureSpeechKey/v1
- Chinh sach rieng tu va ky so: CODE_SIGNING_POLICY.md tren GitHub.
- Giay phep: thu muc licenses\. Ma nguon: https://github.com/tiepcons/ebook-reader

[English]
- Run EbookReader.exe (portable, no installer).
- Put your books (PDF, EPUB, MOBI, ...) in the ebooks\ folder next to EbookReader.exe.
- Optional offline read-aloud with Piper: extract $name-piper-pack.zip into the same folder, then download a voice
  (.onnx and .onnx.json) into data\voices\ yourself (see VOICES below). The pack contains no voices.
- This alpha may be unsigned or self-signed, so Windows SmartScreen may warn.
- Uninstall: delete the app folder; delete data\ next to EbookReader.exe (or %LOCALAPPDATA%\EbookReader when the app folder
  is not writable); if you used Azure Speech, also run: cmdkey /delete:EbookReader.tiepcons/AzureSpeechKey/v1
- Privacy and code signing policy: CODE_SIGNING_POLICY.md on GitHub (no telemetry; network only if you enable Azure Speech).
- Licenses: licenses\ folder. Source code: https://github.com/tiepcons/ebook-reader

VOICES (download both files of a voice into data\voices\; links are pinned to a fixed revision)
- Vietnamese: vi_VN-vais1000-medium
    $hf/vi/vi_VN/vais1000/medium/vi_VN-vais1000-medium.onnx
    $hf/vi/vi_VN/vais1000/medium/vi_VN-vais1000-medium.onnx.json
  License CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/). Attribution: trained on the VAIS-1000 Vietnamese
  speech synthesis corpus (https://ieee-dataport.org/documents/vais-1000-vietnamese-speech-synthesis-corpus, authors as
  credited there); Piper voice published in rhasspy/piper-voices, finetuned from the en_US lessac voice. Changes: converted
  and finetuned for Piper.
- English: en_US-ljspeech-medium (LJ Speech dataset, public domain, https://keithito.com/LJ-Speech-Dataset/)
    $hf/en/en_US/ljspeech/medium/en_US-ljspeech-medium.onnx
    $hf/en/en_US/ljspeech/medium/en_US-ljspeech-medium.onnx.json
- en_US-lessac-medium only if you choose it yourself: it is trained on the Blizzard 2013 Lessac data, whose license
  (https://www.cstr.ed.ac.uk/projects/blizzard/2013/lessac_blizzard2013/license.html) you must read and accept.
    $hf/en/en_US/lessac/medium/en_US-lessac-medium.onnx
    $hf/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json
"@ | Set-Content "$Stage/README.txt" -Encoding utf8
}

function Write-BasicDocs([string]$Stage) {
    $lic = "$Stage/licenses"
    Copy-To "$root/LICENSE" "$lic/LICENSE-EbookReader-AGPL-3.0.txt"
    Copy-To "$root/third_party/mupdf/COPYING" "$lic/LICENSE-MuPDF-AGPL-3.0.txt"
    Copy-To "$root/third_party/libmobi/COPYING" "$lic/LICENSE-libmobi-LGPL-3.0.txt"
    Copy-To "$root/fonts/Literata/OFL.txt" "$lic/OFL-Literata.txt"
    Copy-To "$root/fonts/BeVietnamPro/OFL.txt" "$lic/OFL-BeVietnamPro.txt"
    Copy-To "$root/third_party/mupdf/thirdparty/lcms2/plugins/fast_float/COPYING.GPL3" "$lic/LICENSE-GPL-3.0.txt"
    Copy-To "$root/third_party/mupdf/thirdparty/libjpeg/README" "$lic/LICENSE-libjpeg-IJG-README.txt"
    Copy-To "$root/third_party/mupdf/resources/fonts/urw/OFL.txt" "$lic/OFL-URW-Base14-Nimbus.txt"
    Copy-To "$root/third_party/mupdf/resources/fonts/sil/OFL.txt" "$lic/OFL-CharisSIL.txt"
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
  libjpeg (IJG): this software is based in part on the work of the Independent JPEG Group (licenses\LICENSE-libjpeg-IJG-README.txt)
  fonts embedded in eb_mupdf.dll: URW++ Base14 (Nimbus, Dingbats, ...) and Charis SIL, both SIL OFL 1.1
  (cmark-gfm is part of the MuPDF tree but is not linked into eb_mupdf.dll: Markdown support is disabled)
libmobi               0.12          LGPL-3.0-or-later        https://github.com/bfabiszewski/libmobi
  (LGPL-3.0 is supplemented by the GPL-3.0 text: licenses\LICENSE-GPL-3.0.txt)
miniz (inside libmobi) 1.15         Unlicense / public domain https://github.com/richgel999/miniz
SQLite                3.54.1        Public domain            https://sqlite.org
Literata (font)       google/fonts  SIL OFL 1.1              https://github.com/google/fonts/tree/main/ofl/literata
Be Vietnam Pro (font) google/fonts  SIL OFL 1.1              https://github.com/google/fonts/tree/main/ofl/bevietnampro

LGPL-3.0 note (libmobi): libmobi is compiled statically into EbookReader.exe. You may modify libmobi and relink:
third_party/libmobi in the repository above is the unmodified upstream tree at the version listed, and the repository
contains the build scripts that produce EbookReader.exe. MuPDF is shipped as the separate libs\eb_mupdf.dll, built
from third_party/mupdf with tools/build_mupdf.ps1.
'@ | Set-Content "$lic/THIRD-PARTY-NOTICES.txt" -Encoding utf8
    Write-Readme $Stage
}

function Build-Basic {
    $rel = Join-Path $root 'build/release'
    $exe = Join-Path $rel 'EbookReader.exe'
    $dll = Join-Path $rel 'libs/eb_mupdf.dll'
    foreach ($f in $exe, $dll) { if (-not (Test-Path $f)) { throw "missing $f" } }
    New-Item -ItemType Directory -Force $stage, "$stage/ebooks" | Out-Null
    Copy-To $exe "$stage/EbookReader.exe"
    Copy-To $dll "$stage/libs/eb_mupdf.dll"
    if (-not $NoSign) {
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
    New-Item -ItemType Directory $pk, $ex, "$pk/libs", "$pk/licenses/piper-pack" | Out-Null
    Expand-Safe (Get-Pinned 'piper') $ex
    if (-not (Test-Path "$ex/piper/piper.exe")) { throw 'piper.exe missing in archive' }
    Move-Item "$ex/piper" "$pk/libs/piper"
    Remove-Item $ex -Recurse
    $pl = "$pk/licenses/piper-pack"
    Copy-Item (Get-Pinned 'piperLic') "$pl/LICENSE-piper-MIT.txt"
    Copy-Item (Get-Pinned 'espeakLic') "$pl/LICENSE-espeak-ng-GPL-3.0.txt"
    Copy-Item (Get-Pinned 'ortLic') "$pl/LICENSE-onnxruntime-MIT.txt"
    Copy-Item (Get-Pinned 'ortNotices') "$pl/onnxruntime-ThirdPartyNotices.txt"
    Copy-Item (Get-Pinned 'phonemizeLic') "$pl/LICENSE-piper-phonemize-MIT.txt"
    Copy-Item (Get-Pinned 'tashkeelLic') "$pl/LICENSE-libtashkeel-MIT.txt"
    @'
THIRD-PARTY NOTICES - Piper pack (optional; unmodified downloads from the URLs below)
======================================================================================
Component                    Version        License     Source
---------------------------  -------------  ----------  ------------------------------------------------------
piper                        2023.11.14-2   MIT         https://github.com/rhasspy/piper/releases/tag/2023.11.14-2
espeak-ng (inside piper)     rhasspy fork   GPL-3.0     https://github.com/rhasspy/espeak-ng/tree/0f65aa301e0d6bae5e172cc74197d32a6182200f
    (piper's build fetched the fork's master branch; this is its newest commit before the 2023-11-14 build, approx.)
piper-phonemize (in piper)   approx. fccd4f3  MIT       https://github.com/rhasspy/piper-phonemize/tree/fccd4f335aa68ac0b72600822f34d84363daa2bf
    (newest master commit before the 2023-11-14 build; piper fetched master)
libtashkeel (libtashkeel_model.ort) -           MIT       https://github.com/mush42/libtashkeel
onnxruntime (inside piper)   1.14.1         MIT         https://github.com/microsoft/onnxruntime/tree/v1.14.1
    (version confirmed from onnxruntime.dll version resource 1.14.20230224.5.c57cf37; its third-party notices are included)

No voice models are included. See README.txt in the basic package for voice downloads and their licenses.

Piper runs as a separate process (libs\piper\piper.exe); Ebook Reader only starts it and exchanges text and audio.
espeak-ng is GPL-3.0: its source is available at the URL above. Ebook Reader's own source:
https://github.com/tiepcons/ebook-reader
'@ | Set-Content "$pl/THIRD-PARTY-NOTICES-piper-pack.txt" -Encoding utf8
    $zip = Join-Path $dist "$name-piper-pack.zip"
    New-Zip $zip $pk @(Get-ChildItem $pk -Recurse -File | ForEach-Object FullName) @('data/voices')
    Remove-Item $pk -Recurse -Force
    $zip
}

$suffix = if ($Version -match '^\d+\.\d+\.\d+(-.+)$') { $Matches[1] } else { '' }
$b = & "$PSScriptRoot/build.ps1" -Preset release -VersionSuffix $suffix
if ($LASTEXITCODE -ne 0) { $b; throw 'release build failed' }
foreach ($f in 'EbookReader.exe', 'libs/eb_mupdf.dll') {
    $pv = (Get-Item (Join-Path $root "build/release/$f")).VersionInfo.ProductVersion
    if ($pv -ne $Version) { throw "$f has ProductVersion '$pv' but -Version is '$Version'; bump project(VERSION) in CMakeLists.txt" }
}
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
$zips = @((Build-Basic), (Build-PiperPack))
foreach ($z in $zips) { 'ARTIFACT {0} {1:N2} MB' -f (Split-Path $z -Leaf), ((Get-Item $z).Length / 1MB) }
