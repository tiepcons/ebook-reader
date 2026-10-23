# Ebook Reader

[![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](LICENSE)
![Platform: Windows x64](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-lightgrey)
![Status: in development](https://img.shields.io/badge/status-in%20development-orange)

**Tiếng Việt** · [English](#english)

Trình đọc ebook mã nguồn mở cho Windows x64: nhanh, gọn, nhẹ, ít crash. Viết bằng C++23 + Win32 + Direct2D, dùng MuPDF và libmobi.

> ⚠️ Dự án đang trong giai đoạn phát triển, chưa có bản phát hành. Tiến độ ở [docs/PLAN.md](docs/PLAN.md).

- Định dạng: EPUB, PDF, MOBI, AZW3 (không DRM), FB2, CBZ, TXT (tự nhận UTF-8 / UTF-16 / Windows-1258)
- Thư viện tự quét thư mục `ebooks\` (cạnh file exe, gồm cả thư mục con), có ảnh bìa
- Nhớ vị trí đang đọc. Theme Trắng / Đen / Giấy sách / tự chỉnh. Font theo sách hoặc tự chọn
- Đọc to (TTS) tiếng Anh và tiếng Việt: giọng Windows, Piper (offline), Azure Speech (nhập API key, có đếm % free)
- Thu nhỏ xuống khay hệ thống, chạy nền

## English

A free, open-source ebook reader for Windows x64 focused on being **fast, small and crash-resistant**.
Native C++23, pure Win32 + Direct2D, no UI framework. Rendering by MuPDF; Kindle formats via libmobi.

> ⚠️ Work in progress — no release yet. Progress is tracked in [docs/PLAN.md](docs/PLAN.md) (Vietnamese).

- Formats: EPUB, PDF, MOBI, AZW3 (DRM-free), FB2, CBZ, TXT (UTF-8 / UTF-16 / Windows-1258 auto-detected)
- Library built automatically from the `ebooks\` folder next to the exe (recursive), with covers
- Remembers your reading position; white / dark / sepia / custom themes; publisher fonts or your own
- Text-to-speech in English and Vietnamese: Windows voices, Piper (offline), optional Azure Speech with a free-tier usage meter
- Minimise to tray and keep reading aloud in the background

## Build

Requires Visual Studio 2022 (*Desktop development with C++* + *C++ Clang tools*).

```powershell
git clone --recurse-submodules https://github.com/tiepcons/ebook-reader.git
cd ebook-reader
tools\build.ps1 -Preset release
tools\test.ps1  -Preset debug
```

## Contributing / Đóng góp

See [CONTRIBUTING.md](CONTRIBUTING.md) and the [Code of Conduct](CODE_OF_CONDUCT.md).
Security issues: [SECURITY.md](SECURITY.md) (please report privately).

## License

[AGPL-3.0](LICENSE). Third-party components: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
