# Ebook Reader

Trình đọc ebook mã nguồn mở cho Windows x64: nhanh, gọn, nhẹ. Viết bằng C++20 + Win32 + Direct2D, dùng MuPDF.

- Định dạng: EPUB, PDF, MOBI, AZW3, FB2, CBZ, TXT
- Thư viện tự quét thư mục `ebooks\` (cạnh file exe, gồm cả thư mục con), có ảnh bìa
- Nhớ vị trí đang đọc. Theme Trắng / Đen / Giấy sách / tự chỉnh. Font theo sách hoặc tự chọn
- Đọc to (TTS) tiếng Anh và tiếng Việt: giọng Windows, Piper (offline), Azure Speech (nhập API key, có đếm % free)
- Thu nhỏ xuống khay hệ thống, chạy nền

Kế hoạch và trạng thái: [docs/PLAN.md](docs/PLAN.md).

## Build

Cần Visual Studio 2022 trở lên (workload *Desktop development with C++* + *C++ Clang tools*).

```powershell
tools\build.ps1 -Preset release
tools\test.ps1  -Preset debug
```

## License

[AGPL-3.0](LICENSE). Thư viện bên thứ ba: xem `licenses\`.
