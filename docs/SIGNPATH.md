# Ký số miễn phí với SignPath Foundation — hướng dẫn cho PO

Nguồn chính thức: [Điều khoản SignPath Foundation](https://signpath.org/terms) ·
[Đơn đăng ký](https://signpath.org/apply.html) ·
[Tích hợp GitHub Actions](https://docs.signpath.io/trusted-build-systems/github).
SignPath có thể đổi nội dung form. Nếu khác tài liệu này thì làm theo trang của SignPath.

## 1. Tóm tắt

- SignPath Foundation ký số **miễn phí** cho dự án mã nguồn mở.
- Chứng chỉ đứng tên **"SignPath Foundation"**, không phải tên cá nhân.
- File phải được **build trên GitHub Actions** (runner do GitHub host). SignPath kiểm tra file thật sự build từ repo (origin verification).
- **Mỗi lần ký phải có người duyệt tay**: anh/chị bấm Approve trên SignPath.
- Đơn được xét theo quyết định của SignPath. Dự án mới, ít người dùng có thể phải chờ lâu hoặc bị từ chối.
  Phương án dự phòng: **Azure Trusted Signing**, khoảng $9,99/tháng. Cần kiểm tra cá nhân ở Việt Nam có đăng ký được không.

## 2. Đối chiếu điều khoản SignPath

| Điều khoản (signpath.org/terms) | Repo đã làm | PO cần làm |
|---|---|---|
| "OSI-approved license **without commercial dual-licensing for all components**" | ⚠️ **Xung đột tiềm ẩn lớn nhất:** MuPDF (cả jbig2dec, extract) là AGPL **và** Artifex bán giấy phép thương mại. Ta chỉ dùng theo AGPL, nhưng câu chữ điều khoản áp cho "all components". SQLite (public domain) và libjpeg (giấy phép IJG) không nằm trong danh sách OSI | **Hỏi SignPath trước** (mục 4). Nếu bị từ chối vì MuPDF → phương án dự phòng ở mục 1 |
| "No proprietary component … you may include System Libraries" | ✅ Gói chính: CRT tĩnh `/MT` (System Library). Gói Piper/VieNeu (không ký, tải riêng) kèm VC++ runtime DLL của Microsoft | Hỏi kèm trong mục 4 |
| Không malware, không phần mềm không mong muốn | ✅ | — |
| **Đã phát hành công khai** đúng dạng sẽ ký ("must already be released in the form that should be signed") | ⬜ Chưa có GitHub Release nào | Đăng **một bản alpha prerelease** (chưa ký, ví dụ `v0.9.0-alpha.11`) lên GitHub Releases **trước khi nộp đơn**; không cần beta (mục 6) |
| Đang được bảo trì | ✅ | — |
| Trang tải mô tả chức năng | ✅ README.md | Release notes mô tả ngắn chức năng |
| Chỉ ký binary build từ mã nguồn của mình | ✅ Chỉ ký `EbookReader.exe` và `libs\eb_mupdf.dll`. Piper, onnxruntime, espeak-ng **không ký** | — |
| Riêng tư: chuyển dữ liệu ra ngoài phải có chính sách và cách tắt | ✅ Chỉ khi người dùng tự bật Azure. Azure mặc định tắt, tắt lúc nào cũng được. Có link chính sách riêng tư của Microsoft | — |
| Báo trước thay đổi hệ thống | ✅ App không ghi registry, không tự khởi động, không đăng ký loại file | — |
| Có hướng dẫn gỡ cài đặt | ✅ README.md, README.txt trong zip, CODE_SIGNING_POLICY.md | — |
| **MFA** cho mọi thành viên trên GitHub và SignPath | ✅ GitHub (PO bật 2026-10-30) | Bật MFA trên SignPath khi có tài khoản |
| Riêng tư: "display this policy during installation" | ✅ App portable, không có bộ cài: thông báo riêng tư hiện **trong app trước khi lưu khóa Azure** (phải bấm Đồng ý), và có trong `CODE_SIGNING_POLICY.md`, README | — |
| Danh tiếng / quyền kiểm soát dự án được xét lúc duyệt | ⚠️ Repo mới, 0 sao, chưa có release | Có release alpha công khai, README rõ ràng; có thể phải chờ hoặc bị từ chối |
| Phân vai Authors / Reviewers / Approvers | ✅ Cả ba vai là tiepcons (ghi trong `CODE_SIGNING_POLICY.md`) | — |
| Trang **Code signing policy** ở trang chủ, link từ trang tải | ✅ `CODE_SIGNING_POLICY.md` có link trong README | Thêm link vào **mỗi** release notes |
| Metadata: ProductName và ProductVersion giống nhau ở mọi file | ✅ VERSIONINFO: `Ebook Reader` và `0.9.0-…` ở cả exe lẫn dll | — |
| Build kiểm chứng được từ mã nguồn, mỗi lần ký duyệt tay | ✅ `.github/workflows/release.yml` chỉ chạy khi push tag `v*`, runner `windows-2022` | Bấm Approve trên SignPath mỗi lần release |

## 3. Thông tin điền form (copy-paste, tiếng Anh)

| Trường | Giá trị |
|---|---|
| Project name | `Ebook Reader` |
| Repository URL | `https://github.com/tiepcons/ebook-reader` |
| Homepage | `https://github.com/tiepcons/ebook-reader#readme` |
| Download / release page | `https://github.com/tiepcons/ebook-reader/releases` |
| Code signing policy URL | `https://github.com/tiepcons/ebook-reader/blob/main/CODE_SIGNING_POLICY.md` |
| License | `AGPL-3.0-or-later` (LICENSE file in repo root) |
| Maintainer / contact | tiepcons — *(email anh/chị muốn SignPath liên hệ)* |
| Build system | GitHub Actions, GitHub-hosted runner `windows-2022`, workflow `.github/workflows/release.yml`, triggered by tags `v*` only |
| Artifacts to sign | Portable ZIP `EbookReader-<version>-win-x64.zip` containing `EbookReader.exe` and `libs/eb_mupdf.dll` (Authenticode). No installer. |
| Number of users | *(trung thực: dự án mới, ghi số sao và lượt tải lúc nộp)* |

**Project description** (dán nguyên đoạn):

> Ebook Reader is a free, open-source (AGPL-3.0) native ebook reader for Windows x64, written in C++ with Win32 and Direct2D.
> It opens EPUB, MOBI/AZW3, FB2, PDF, CBZ and TXT through MuPDF and libmobi. It keeps a library with covers indexed from a local
> `ebooks` folder and remembers the reading position. It can read aloud in Vietnamese and English using offline Windows voices or
> Piper; Azure Speech is optional and only used with the user's own API key. It has no telemetry and no auto-update. It is distributed as a portable ZIP
> on GitHub Releases. We want to sign our own binaries (EbookReader.exe and eb_mupdf.dll, built from this repository on
> GitHub-hosted runners) so users stop getting SmartScreen "unknown publisher" warnings.

## 4. Ba điểm nên hỏi SignPath (dán vào ô ghi chú, hoặc email support@signpath.io trước khi nộp)

> 1. Your terms require an OSI license "without commercial dual-licensing for all components". Our document engine is MuPDF
>    (AGPL-3.0; Artifex also sells commercial licenses). We use MuPDF only under the AGPL, as an unmodified git submodule
>    built into our own `eb_mupdf.dll` together with our C wrapper, and the whole project is AGPL-3.0-or-later with no
>    commercial edition of our own. Is a dependency that its upstream dual-licenses acceptable? If not, may we sign only
>    `EbookReader.exe` and ship `eb_mupdf.dll` unsigned?
> 2. We statically link SQLite (public domain) and MuPDF bundles libjpeg (IJG license); neither is on the OSI list. Is that acceptable?
> 3. Our optional, unsigned voice packs (separate downloads) include the Microsoft Visual C++ runtime DLLs needed by
>    upstream binaries. We treat them as System Libraries. Is that acceptable?

## 5. Meta đã có trong file (VERSIONINFO)

| Trường | EbookReader.exe | libs\eb_mupdf.dll |
|---|---|---|
| ProductName | Ebook Reader | Ebook Reader |
| ProductVersion | ví dụ `0.9.0-beta.1`, lấy từ tag `v0.9.0-beta.1` | giống exe |
| FileVersion | 0.9.0.0 | 0.9.0.0 |
| FileDescription | Ebook Reader | Ebook Reader document engine (MuPDF) |
| OriginalFilename | EbookReader.exe | eb_mupdf.dll |
| CompanyName | Ebook Reader contributors | giống exe |
| LegalCopyright | AGPL-3.0-or-later, Ebook Reader contributors | giống exe |

Phiên bản lấy từ `project(VERSION)` trong CMake, cộng hậu tố từ tag. `package.ps1` báo lỗi nếu ProductVersion lệch với tag.

## 6. Thứ tự làm (khớp với PLAN)

1. ⬜ Anh/chị test alpha theo `docs/ACCEPTANCE.md`, em sửa lỗi phát sinh.
2. ✅ Soak, fuzz (Phase 8), CI local đạt, `main` đã push.
3. ✅ **T9.5** build Piper từ mã nguồn.
4. ✅ **MFA GitHub** (PO bật 2026-10-30).
5. ⬜ *(Khuyến nghị, PO duyệt)* GitHub ruleset: chặn force-push và xoá `main`; chỉ tiepcons được tạo/sửa/xoá tag `v*`.
6. ⬜ **Phát hành công khai một bản alpha** (điều kiện "already released"): push tag `v0.9.0-alpha.N`. Workflow build trên
   GitHub-hosted runner và tạo **release nháp prerelease** (chưa ký). Ghi chú release có mô tả chức năng và link Code signing policy.
   Anh/chị xem lại rồi bấm **Publish**. Không cần beta.
7. ⬜ Nộp đơn tại https://signpath.org/apply.html bằng thông tin mục 3 và **cả 3 câu hỏi ở mục 4**.
8. ⬜ Khi được duyệt, làm mục 7.

## 7. Sau khi SignPath duyệt (cấu hình một lần)

Trên **SignPath** (app.signpath.io):
1. Bật MFA cho tài khoản.
2. **Trusted Build Systems**: thêm **GitHub.com** và liên kết với project. Cài **SignPath GitHub App** cho repo `ebook-reader`.
3. Tạo **Project**, slug đề xuất: `ebook-reader`.
4. **Artifact configuration**: dán nội dung `.signpath/artifact-configuration.xml` trong repo.
5. **Signing policy**, slug đề xuất: `release-signing`. Gắn chứng chỉ của SignPath Foundation, bật **manual approval**, Approver là tiepcons.
6. Tạo một người dùng CI hoặc API token có quyền **Submitter** cho policy trên. Chép token.

Trên **GitHub** (repo → Settings → Secrets and variables → Actions):

| Loại | Tên | Giá trị |
|---|---|---|
| Secret | `SIGNPATH_API_TOKEN` | token vừa chép (không dán vào đâu khác) |
| Variable | `SIGNPATH_ORGANIZATION_ID` | Organization ID trên SignPath |
| Variable | `SIGNPATH_PROJECT_SLUG` | `ebook-reader` |
| Variable | `SIGNPATH_SIGNING_POLICY_SLUG` | `release-signing` |

Khuyến nghị thêm (một lần):

| Nơi | Cấu hình |
|---|---|
| SignPath | Pipeline policy `github-build-policies` với `runners: required_github_hosted` cho signing policy `release-signing` |
| GitHub | Tag ruleset cho `v*`: chỉ tiepcons được tạo/sửa/xóa tag |

Từ đó, mỗi lần push tag `v*`: workflow build → gửi yêu cầu ký → anh/chị bấm **Approve** trên SignPath → release nháp có zip đã ký → **Publish**.
Nếu biến `SIGNPATH_ORGANIZATION_ID` còn trống, workflow tự bỏ qua bước ký.

## 8. Lưu ý

- Không ký file của bên thứ ba (Piper, onnxruntime, espeak-ng). Điều khoản SignPath cho phép kèm file chưa ký của upstream.
- Chứng chỉ tự ký dùng cho bản alpha local (`tools/sign.ps1`) **không liên quan** tới SignPath và không dùng cho bản công khai.
- Nếu SignPath thu hồi chứng chỉ (vi phạm điều khoản), các bản đã ký có thể mất hiệu lực. Vì vậy phải giữ đúng mọi mục ở bảng 2 cho mỗi lần release.
