# Releasing (local alpha)

No GitHub Actions: everything runs locally and nothing is uploaded or tagged automatically.

1. `tools/release.ps1 -Version 0.9.0-alpha.1` (add `-SkipCi` to skip `tools/ci.ps1`).
   It builds the release preset, stages `dist/EbookReader-<ver>/`, signs `EbookReader.exe` and `libs\eb_mupdf.dll`
   with `tools/sign.ps1`, writes `dist/EbookReader-<ver>-win-x64.zip` (must be <= 10 MB),
   `dist/EbookReader-<ver>-piper-pack.zip` and `dist/SHA256SUMS.txt`.
2. Piper pack inputs (piper 2023.11.14-2 and license texts for piper, piper-phonemize, libtashkeel, espeak-ng fork,
   onnxruntime) are pinned by URL and SHA256 in `tools/package.ps1`, cached in `build/downloads/`, extracted but never
   executed. To bump a pin, change URL and hash together after reviewing the new file.
   Voices are NOT bundled (PLAN D19). The basic package README.txt tells users to download `.onnx` + `.onnx.json` into
   `data\voices\`: vi_VN-vais1000-medium (CC BY 4.0, attribution in README), en_US-ljspeech-medium (public-domain
   dataset), and en_US-lessac-medium only by explicit user choice (Blizzard 2013 license). Links point to a fixed
   piper-voices commit, not `main`.
3. Signing uses a self-signed certificate `CN=Ebook Reader (self-signed alpha)` in `Cert:\CurrentUser\My`, created
   on first use with a non-exportable key. Windows reports its status as `UnknownError` (untrusted root) and
   SmartScreen may warn. `tools/sign.ps1 -Trust` adds the public certificate to CurrentUser Root on your own machine
   (shows a Windows dialog); never run it in automation. Only our own binaries under `dist\` are signed, never piper/onnxruntime.
4. Publishing is manual: `git tag`, upload the zips and `SHA256SUMS.txt` yourself.

Public releases: apply for free code signing through the SignPath Foundation (OSS program) and replace the
self-signed step in `tools/sign.ps1`; the unsigned-chain warning goes away once the certificate chains to a public CA.
