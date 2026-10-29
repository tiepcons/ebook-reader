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

$piperPins = Import-PowerShellDataFile "$PSScriptRoot/piper_pins.psd1"
$pins = @{ tashkeelLic = @($piperPins.TashkeelLicense.Url, $piperPins.TashkeelLicense.Sha256) }

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
    $out = Join-Path $root 'build/piper-src/out'
    if (-not (Test-Path "$out/piper.exe")) {
        & "$PSScriptRoot/build_piper.ps1" | Out-Host
        if (-not (Test-Path "$out/piper.exe")) { throw 'build_piper.ps1 failed' }
    }
    $sr = Join-Path $root 'build/piper-src/src'
    $ort = Join-Path $root "build/piper-src/ort/$($piperPins.Onnxruntime.Dir)"
    $pk = Join-Path $dist "$name-piper-pack-stage"
    $pl = "$pk/licenses/piper-pack"
    New-Item -ItemType Directory $pk, "$pk/libs", $pl | Out-Null
    Copy-Item $out "$pk/libs/piper" -Recurse
    Copy-Item "$sr/piper/LICENSE.md" "$pl/LICENSE-piper-MIT.txt"
    Copy-Item "$sr/espeak/COPYING" "$pl/LICENSE-espeak-ng-GPL-3.0.txt"
    Copy-Item "$sr/sonic/LICENSE" "$pl/LICENSE-sonic-Apache-2.0.txt"
    Copy-Item "$sr/phonemize/LICENSE.md" "$pl/LICENSE-piper-phonemize-MIT.txt"
    Copy-Item "$sr/fmt/LICENSE.rst" "$pl/LICENSE-fmt-MIT.txt"
    Copy-Item "$sr/spdlog/LICENSE" "$pl/LICENSE-spdlog-MIT.txt"
    Copy-Item "$ort/LICENSE" "$pl/LICENSE-onnxruntime-MIT.txt"
    Copy-Item "$ort/ThirdPartyNotices.txt" "$pl/onnxruntime-ThirdPartyNotices.txt"
    Copy-Item (Get-Pinned 'tashkeelLic') "$pl/LICENSE-libtashkeel-MIT.txt"
    Write-PiperNotices "$pl/THIRD-PARTY-NOTICES-piper-pack.txt"
    $zip = Join-Path $dist "$name-piper-pack.zip"
    New-Zip $zip $pk @(Get-ChildItem $pk -Recurse -File | ForEach-Object FullName) @('data/voices')
    Remove-Item $pk -Recurse -Force
    $zip
}

function Write-PiperNotices([string]$Path) {
    $s = $piperPins.Sources
    function Row([string]$Label, [string]$Lic, [string]$Key) { '{0,-30}{1,-12}{2}/tree/{3}' -f $Label, $Lic, $s[$Key].Url, $s[$Key].Commit }
    @"
THIRD-PARTY NOTICES - Piper pack (optional; built from source by Ebook Reader's tools/build_piper.ps1)
=====================================================================================================
Every component below is built from the exact git commit shown (and onnxruntime is the unmodified official 1.14.1
release); tools/piper_pins.psd1 in the Ebook Reader repository pins them. Source URL = repository/tree/commit.

Component                     License     Source (exact commit)
----------------------------  ----------  ------------------------------------------------------------------
$(Row 'piper 2023.11.14-2' 'MIT' 'piper')
$(Row 'piper-phonemize 2023.11.14-4' 'MIT' 'phonemize')
$(Row 'espeak-ng (rhasspy fork)' 'GPL-3.0' 'espeak')
$(Row 'sonic (inside espeak-ng.dll)' 'Apache-2.0' 'sonic')
$(Row 'fmt 10.0.0 (in piper.exe)' 'MIT' 'fmt')
$(Row 'spdlog 1.12.0 (in piper.exe)' 'MIT' 'spdlog')
libtashkeel_model.ort         MIT         https://github.com/mush42/libtashkeel
onnxruntime 1.14.1            MIT         https://github.com/microsoft/onnxruntime/releases/tag/v1.14.1
                                          (official win-x64 binary, SHA256 $($piperPins.Onnxruntime.Sha256); its third-party notices are included)

espeak-ng is GPL-3.0: the corresponding source of the espeak-ng.dll in this pack is the commit above (plus sonic at its
commit). Build instructions: tools/build_piper.ps1 and tools/piper_pins.psd1 in https://github.com/tiepcons/ebook-reader .
The only change to upstream sources is stripping the trailing newline of piper/VERSION.

No voice models are included. See README.txt in the basic package for voice downloads and their licenses.

Piper runs as a separate process (libs\piper\piper.exe); Ebook Reader only starts it and exchanges text and audio.
Ebook Reader's own source: https://github.com/tiepcons/ebook-reader
"@ | Set-Content $Path -Encoding utf8
}

$suffix = if ($Version -match '^\d+\.\d+\.\d+(-.+)$') { $Matches[1] } else { '' }
$b = & "$PSScriptRoot/build.ps1" -Preset release -VersionSuffix $suffix
if ($LASTEXITCODE -ne 0) { $b; throw 'release build failed' }
foreach ($f in 'EbookReader.exe', 'libs/eb_mupdf.dll') {
    $pv = (Get-Item (Join-Path $root "build/release/$f")).VersionInfo.ProductVersion
    if ($pv -ne $Version) { throw "$f has ProductVersion '$pv' but -Version is '$Version'; bump project(VERSION) in CMakeLists.txt" }
}
Get-ChildItem $dist -Filter "EbookReader-$Version*" -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force # keep other versions the PO may be testing
$zips = @((Build-Basic), (Build-PiperPack))
foreach ($z in $zips) { 'ARTIFACT {0} {1:N2} MB' -f (Split-Path $z -Leaf), ((Get-Item $z).Length / 1MB) }
