# Dot-sourced by package.ps1: builds EbookReader-<ver>-vieneu-pack.zip (audio.cpp CPU runtime + sea-g2p library, licences
# and a README that tells the user where to download the 188 MB model, sea_g2p.bin and the voices). The model is not bundled.
# Uses $vieneuPins, $dist, $name, $Version and the helpers of package.ps1.

function Write-VieNeuReadme([string]$Path) {
    $p = $vieneuPins
    $files = ($p.Voices.Files.GetEnumerator() | Sort-Object Key | ForEach-Object { '    {0}  {1}' -f $_.Value, $_.Key }) -join "`r`n"
    $ids = ($p.Voices.Files.Keys | Sort-Object | ForEach-Object { ($_ -split '/')[0] } | Select-Object -Unique | ForEach-Object { "'$_'" }) -join ', '
    $text = @'
VIENEU PACK - EBOOK READER @@VERSION@@ (optional, offline read-aloud voices)

[Tieng Viet]
Goi nay chi chua chuong trinh chay (libs\vieneu\) va giay phep. Mo hinh giong noi (khoang 188 MB), tu dien sea_g2p.bin
(khoang 63 MB) va cac giong mau KHONG kem theo: ban tu tai vao data\vieneu\ canh EbookReader.exe (xem ben duoi), kiem tra
SHA256, roi khoi dong lai EbookReader. Trong Cai dat doc to se co them bo may "VieNeu". Thieu hoac hong tep nao thi bo may
nay tu an, ung dung van chay binh thuong. Can khoang 1 GB RAM va CPU 4 nhan tro len de doc nhanh hon toc do noi.

[English]
This pack only holds the runtime (libs\vieneu\) and licenses. The voice model (about 188 MB), the sea_g2p.bin dictionary
(about 63 MB) and the voice presets are NOT included: download them into data\vieneu\ next to EbookReader.exe (see below),
verify the SHA256, then restart Ebook Reader. The read-aloud settings then list a "VieNeu" engine. If any file is missing or
damaged the engine stays hidden and the rest of the app is unaffected. It needs about 1 GB of RAM.

FILES TO DOWNLOAD (links are pinned to a fixed revision; all are Apache-2.0)
  data\vieneu\vieneu-v3-turbo-q8_0.gguf      @@MODEL_SIZE@@ bytes
    @@MODEL_URL@@
    SHA256 @@MODEL_SHA@@
  data\vieneu\sea_g2p.bin                    @@DICT_SIZE@@ bytes
    @@DICT_URL@@
    SHA256 @@DICT_SHA@@
  data\vieneu\voices\manifest.json
    @@VOICE_BASE@@/manifest.json
    SHA256 @@MANIFEST_SHA@@
  data\vieneu\voices\<id>\speaker.emb.txt and ref_codes.txt for each voice you want (any subset works):
    @@VOICE_BASE@@/<id>/<file>
    SHA256 of every file:
@@VOICE_FILES@@

POWERSHELL (run in the folder that holds EbookReader.exe; downloads everything)
  $d = 'data\vieneu'; New-Item -ItemType Directory -Force "$d\voices" | Out-Null
  Invoke-WebRequest '@@MODEL_URL@@' -OutFile "$d\vieneu-v3-turbo-q8_0.gguf"
  Invoke-WebRequest '@@DICT_URL@@' -OutFile "$d\sea_g2p.bin"
  Invoke-WebRequest '@@VOICE_BASE@@/manifest.json' -OutFile "$d\voices\manifest.json"
  foreach ($id in @@VOICE_IDS@@) {
    New-Item -ItemType Directory -Force "$d\voices\$id" | Out-Null
    foreach ($f in 'speaker.emb.txt', 'ref_codes.txt') { Invoke-WebRequest "@@VOICE_BASE@@/$id/$f" -OutFile "$d\voices\$id\$f" }
  }
  Get-FileHash "$d\vieneu-v3-turbo-q8_0.gguf", "$d\sea_g2p.bin" | Format-List Hash, Path

Licenses: licenses\vieneu-pack\ (audio.cpp, ggml and the other components of the runtime, sea-g2p). Model: VieNeu-TTS v3 Turbo,
Apache-2.0, https://huggingface.co/pnnbao-ump/VieNeu-TTS-v3-Turbo . Source of Ebook Reader: https://github.com/tiepcons/ebook-reader
'@
    $map = @{
        VERSION = $Version; MODEL_URL = $p.Model.Url; MODEL_SHA = $p.Model.Sha256; MODEL_SIZE = $p.Model.Size
        DICT_URL = $p.SeaG2pDict.Url; DICT_SHA = $p.SeaG2pDict.Sha256; DICT_SIZE = $p.SeaG2pDict.Size
        VOICE_BASE = $p.Voices.BaseUrl; MANIFEST_SHA = $p.Voices.Manifest; VOICE_FILES = $files; VOICE_IDS = $ids
    }
    foreach ($k in $map.Keys) { $text = $text.Replace("@@$k@@", [string]$map[$k]) }
    Set-Content $Path $text -Encoding utf8
}

function Write-VieNeuNotices([string]$Path) {
    @"
THIRD-PARTY NOTICES - VieNeu pack (optional)
============================================
Everything below is redistributed unmodified from the official release assets pinned by SHA256 in tools/vieneu_pins.psd1 of
https://github.com/tiepcons/ebook-reader . Ebook Reader runs libs\vieneu\audiocpp_cli.exe as a separate, low-integrity process;
it only exchanges text and audio files with it. No server or network port is used.

Component                       Version    License       Source
------------------------------  ---------  ------------  ------------------------------------------------------------
audio.cpp (audiocpp_cli.exe)    0.9.3      Apache-2.0    https://github.com/0xShug0/audio.cpp/tree/v0.9.3
  ggml (vendored in audio.cpp)             MIT           licenses\vieneu-pack\LICENSE-ggml-MIT.txt
  cJSON, libyaml                           MIT           LICENSE-cJSON-MIT.txt, LICENSE-libyaml-MIT.txt
  sentencepiece                            Apache-2.0    LICENSE-sentencepiece-Apache-2.0.txt
  utf8proc                                 MIT / Unicode LICENSE-utf8proc.txt
  (audio.cpp's own license text: LICENSE-audio.cpp-Apache-2.0.txt; its external\ folder lists the vendored code)
sea-g2p (sea_g2p_rs.dll)        0.10.0     Apache-2.0    https://github.com/pnnbao97/sea-g2p/tree/v0.10.0
Microsoft Visual C++ runtime    app-local  Microsoft     MSVCP140.dll, VCRUNTIME140.dll, VCOMP140.dll and the other
  (libs\vieneu)                                          VC++ runtime DLLs imported by the pack, redistributed unmodified
                                                         under the Microsoft Visual Studio redistribution terms.

Downloaded by the user, not included in this pack (see README-VieNeu.txt):
VieNeu-TTS v3 Turbo model       Apache-2.0    https://huggingface.co/pnnbao-ump/VieNeu-TTS-v3-Turbo
sea_g2p.bin dictionary          Apache-2.0    https://github.com/pnnbao97/sea-g2p/releases/tag/v0.10.0
Voice presets                   Apache-2.0    same repository as the model, folder gguf/voices
"@ | Set-Content $Path -Encoding utf8
}

function Build-VieNeuPack {
    $p = $vieneuPins
    $pk = Join-Path $dist "$name-vieneu-pack-stage"
    $lib = "$pk/libs/vieneu"
    $lic = "$pk/licenses/vieneu-pack"
    New-Item -ItemType Directory $pk, $lib, $lic | Out-Null
    $za = [IO.Compression.ZipFile]::OpenRead((Get-PinnedUrl $p.AudioCpp.Url $p.AudioCpp.Sha256))
    try {
        foreach ($f in $p.AudioCpp.Files + 'LICENSE') {
            $e = $za.GetEntry($f)
            if (-not $e) { throw "audio.cpp zip has no $f" }
            $dst = if ($f -eq 'LICENSE') { "$lic/LICENSE-audio.cpp-Apache-2.0.txt" } else { Join-Path $lib $f }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dst)
        }
    } finally { $za.Dispose() }
    Copy-Item (Get-PinnedUrl $p.SeaG2pDll.Url $p.SeaG2pDll.Sha256) "$lib/sea_g2p_rs.dll"
    Add-VcRuntime $lib
    foreach ($k in $p.Licenses.Keys | Sort-Object) { Copy-Item (Get-PinnedUrl $p.Licenses[$k][0] $p.Licenses[$k][1]) "$lic/$k" }
    Write-VieNeuNotices "$lic/THIRD-PARTY-NOTICES-vieneu-pack.txt"
    Write-VieNeuReadme "$pk/README-VieNeu.txt"
    $zip = Join-Path $dist "$name-vieneu-pack.zip"
    New-Zip $zip $pk @(Get-ChildItem $pk -Recurse -File | ForEach-Object FullName) @('data/vieneu/voices')
    Remove-Item $pk -Recurse -Force
    $zip
}
