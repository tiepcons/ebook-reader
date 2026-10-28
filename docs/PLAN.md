# KẾ HOẠCH CHI TIẾT: EBOOK READER (Native C++ / Win32 / Windows x64 / Open Source)

> **File này lưu kế hoạch tổng.** Khi quên thì mở lại file này.
> Trạng thái: ⬜ chưa làm · 🟨 đang làm · ✅ xong · ⛔ bị chặn.
> Phiên bản: **v4**, ngày 2026-10-23. Tiêu chí số 1: **NHANH · GỌN · NHẸ · BỀN · ÍT CRASH**.

---

## 0. Tóm tắt 1 phút

Phần mềm đọc ebook **mã nguồn mở (AGPL-3.0)**, viết bằng **C++20 + Win32 thuần + Direct2D**, chạy trên Windows x64.
- **Không dùng framework giao diện.** Chỉ dùng thư viện ngoài khi Windows không có sẵn chức năng đó.
- Tự quét đệ quy thư mục **`ebooks\`** để tạo thư viện có ảnh bìa và cây thư mục.
- Đọc EPUB, PDF, MOBI, AZW3, FB2, CBZ, TXT. **Nhớ chỗ đang đọc** của từng truyện.
- Theme đọc gồm Trắng, Đen, **Giấy sách (hơi vàng)** và Tự chỉnh màu. Font đọc **theo CSS của sách** hoặc **font miễn phí** người dùng tự chọn.
- Bấm X thì hỏi **Thoát / Xuống khay**, có ô ghi nhớ lựa chọn. App chạy nền được.
- TTS tiếng Anh và tiếng Việt qua 3 engine: WinRT (có sẵn, miễn phí), Piper (thư viện tùy chọn, miễn phí), **Azure** (người dùng nhập API key, app **đếm % free còn lại**).
- MuPDF và Piper được **tách thành DLL riêng trong `libs\`**.
- Chi phí: **$0**.

**Mục tiêu đo được:** bản zip cơ bản ≤ 10 MB · khởi động ≤ 0,3 giây · lật trang ≤ 30 ms · RAM khi ở khay ≤ 20 MB · **0 crash** trên toàn bộ corpus test và sau 1 giờ fuzz.

---

## 1. Các quyết định đã chốt (không bàn lại)

| # | Quyết định | Lý do |
|---|---|---|
| D1 | **AGPL-3.0, mở mã nguồn** | Dùng được MuPDF miễn phí |
| D2 | **C++23 (dùng `std::expected`), MSVC 2022 (và clang-cl để fuzz), CMake + Ninja**. **Không dùng vcpkg**: các thư viện ngoài được vendor (git submodule hoặc file amalgamation) | Ít công cụ, build lặp lại được, không phải chờ build Qt |
| D3 | **Giao diện: Win32 thuần + Direct2D/DirectWrite + Common Controls v6.** Không dùng Qt, không dùng WTL. Tự viết vài lớp RAII nhỏ | Nhẹ nhất: exe khoảng 2 MB, RAM khoảng 10 MB |
| D4 | **MuPDF** là engine duy nhất cho mọi định dạng. **TXT cũng được chuyển thành HTML rồi đưa cho MuPDF** | Chỉ có 1 đường render, nên theme, font, vị trí đọc và TTS dùng chung cho mọi định dạng |
| D5 | **SQLite amalgamation** biên dịch thẳng vào exe. Không dùng FTS5: tìm kiếm bằng `LIKE` trên cột đã bỏ dấu | Vài nghìn sách thì `LIKE` vẫn chạy dưới 10 ms |
| D6 | Thư viện = **quét đệ quy `<exe>\ebooks\`** và các thư mục người dùng thêm. Quét lại tăng dần khi khởi động, khi cửa sổ được focus lại, và khi bấm nút. **Không dùng FolderWatcher** | Ít thread, ít bug. Quét lại 1000 sách chỉ khoảng 1 giây |
| D7 | **Chạy portable**: dữ liệu ở `<exe>\data\`. Nếu không ghi được thì dùng `%LOCALAPPDATA%\EbookReader\` | |
| D8 | **Định danh sách = hash** (kích thước + SHA-256 của 64 KB đầu và cuối, dùng BCrypt có sẵn trong Windows) | Đổi tên hoặc di chuyển file vẫn giữ vị trí đọc |
| D9 | **DLL riêng trong `libs\` với C ABI ổn định**: `eb_mupdf.dll` (bắt buộc). **Piper (tùy chọn) chạy dưới dạng tiến trình con**: `libs\piper\piper.exe` (bản build chính thức, MIT), giao tiếp qua stdin/stdout ở chế độ JSON (đổi ngày 2026-10-24 thay cho `eb_piper.dll`) | Cập nhật riêng từng phần. Piper crash thì app không chết. Không phải build onnxruntime/espeak-ng, không lệch CRT. Thiếu Piper thì app vẫn chạy |
| D10 | **CRT tĩnh (`/MT`)** cho exe và mọi DLL. **DLL nào cấp phát bộ nhớ thì DLL đó giải phóng** | Không cần cài VC++ Redistributable |
| D11 | **Theme chỉ áp dụng cho vùng đọc.** Menu và control giữ màu hệ thống. Thanh tiêu đề chuyển tối bằng `DwmSetWindowAttribute` | Không phải dùng hack dark mode của Win32 |
| D12 | **Không hỗ trợ DRM** | Bẻ DRM là vi phạm pháp luật |
| D13 | **Phát hành bằng bản zip portable.** Bộ cài để sang backlog | Gọn nhất |
| D14 | Thư viện ngoài cho phép dùng: **MuPDF, libmobi, SQLite, Piper + onnxruntime + espeak-ng (tùy chọn), doctest (chỉ dùng cho test)**. Muốn thêm thư viện khác thì PO phải duyệt | Chống phình |
| D15 | MuPDF build với **`TOFU_CJK` + `TOFU_NOTO`**, tắt JS/OCR/barcode/Office/XPS/IMG/MD. Tiếng Việt và Latin hiển thị bằng Charis SIL có sẵn, cộng font kèm theo. Chữ khác (Ả Rập, Hindi…) thì thả font vào `fonts\` | `eb_mupdf.dll` giảm từ 16 MB xuống 7,9 MB, ít bề mặt tấn công |
| D16 | **MuPDF không đọc được AZW3/KF8 và MOBI nén HUFF/CDIC** (spike T0.6). **PO đã duyệt libmobi (LGPL-3.0)** (2026-10-23). Nhúng vào `eb_mupdf.dll`: AZW3/KF8/HUFF-CDIC được chuyển sang EPUB trong bộ nhớ rồi mở bằng MuPDF, ABI không đổi (T1.9) | Hỗ trợ đầy đủ định dạng Kindle không DRM |
| D17 | `eb_mupdf` tự mở file (`CreateFileW` + kiểm tra là file trên đĩa), không đưa thư mục cho MuPDF, giới hạn heap 1 GiB, có `eb_abort` để hủy | Kết quả review bảo mật T1.6 |
| D19 | **Piper (spike T0.8)**: bản rhasspy/piper 2023.11.14-2 (MIT, onnxruntime 1.14), RTF khoảng 0,1, gói khoảng 38 MB + khoảng 62 MB mỗi giọng. **Không đóng gói giọng sẵn**: người dùng tải khi cần. Giọng mặc định: VI `vi_VN-vais1000-medium` (CC BY 4.0, ghi công). EN dùng giọng có dataset rõ license (ưu tiên `en_US-ljspeech`, dataset public domain). **Không dùng** `vi_VN-25hours_single` (license "Unknown"). `lessac` chỉ dùng khi người dùng tự chọn, kèm thông báo license | Giọng hay, chạy offline, không vướng license |
| D18 | Ngôn ngữ và ảnh bìa nhúng của EPUB/FB2/MOBI: MuPDF không cung cấp, nên phải tự đọc OPF/EXTH (T2.3–T2.4) | Kết quả spike T0.6 |

---

## 2. Bố cục khi chạy

```
EbookReader\                    ← giải nén zip là chạy
├─ EbookReader.exe              (~2–3 MB: UI, SQLite, TTS WinRT, Azure)
├─ libs\
│  ├─ eb_mupdf.dll              (~8–10 MB, BẮT BUỘC; MuPDF build không kèm font CJK)
│  └─ piper\                    (TÙY CHỌN, giải nén gói Piper vào đây)
│     ├─ eb_piper.dll  onnxruntime.dll  espeak-ng-data\
├─ voices\                      (model Piper: vi_VN, en_US ... *.onnx + *.json)
├─ fonts\                       (Literata, Be Vietnam Pro có sẵn; thả thêm .ttf/.otf vào đây)
├─ ebooks\                      ← NGƯỜI DÙNG BỎ SÁCH VÀO ĐÂY, thư mục con tùy ý
├─ data\
│  ├─ library.db                (sách, tiến độ, bookmark, lượt dùng Azure, cài đặt)
│  ├─ covers\<hash>.jpg         (thumbnail tạo bằng WIC)
│  ├─ tts-cache\                (audio Azure đã tổng hợp)
│  ├─ crash\                    (minidump khi crash)
│  └─ app.log                   (1 MB, xoay vòng 2 file)
└─ licenses\
```

## 3. Bố cục mã nguồn

```
ebook-reader/
├─ CMakeLists.txt  CMakePresets.json  LICENSE  README.md
├─ docs/PLAN.md
├─ third_party/   mupdf/ (submodule, ghim theo tag)  sqlite/ (amalgamation)  doctest/  piper/
├─ libs/
│  ├─ eb_mupdf/   eb_mupdf.h (C ABI)  eb_mupdf.c  → eb_mupdf.dll
│  └─ eb_piper/   eb_piper.h (C ABI)  eb_piper.cpp → eb_piper.dll
├─ src/
│  ├─ base/       Result.h, Log, Paths, Hash, AtomicFile, Handle (RAII), DllLoader, CrashGuard, Text (UTF-8/16, bỏ dấu)
│  ├─ doc/        Document (bọc eb_mupdf), TxtToHtml, Location
│  ├─ lib/        Db, Schema, Scanner, Metadata, CoverCache, ProgressStore
│  ├─ look/       Theme (preset, màu tự chỉnh), FontManager
│  ├─ tts/        ITtsEngine, Segmenter, LangDetect, WinRtTts, PiperTts, AzureTts, AzureUsage, SecretStore
│  └─ ui/         App (WinMain, vòng lặp message), MainWnd, LibraryPane (TreeView + ListView ảo),
│                 ReaderPane (Direct2D), RenderWorker, Tray, CloseAsk, SettingsDlg (.rc), TtsBar
├─ res/           app.rc, app.manifest (PerMonitorV2, longPathAware, ComCtl v6), icon
├─ tests/         unit/ (doctest)  integration/  fuzz/  corpus/
├─ tools/         package.ps1
└─ .github/workflows/ ci.yml  release.yml
```

---

## 4. QUY TẮC CODE: NHANH · GỌN · NHẸ · BỀN

### 4.1 Gọn (bỏ phần rườm rà)
- **Không thêm thư viện** ngoài danh sách D14. Windows đã có gì thì dùng nấy: WIC cho ảnh, WinHTTP cho mạng, BCrypt cho hash, CredMan cho secret, DbgHelp cho dump, `TaskDialogIndirect` cho hộp thoại.
- **Chỉ tạo interface khi có từ 2 cách cài đặt trở lên** (`ITtsEngine` có 3, nên giữ). Không viết factory, manager hay lớp trừu tượng "để dành sau này".
- Hàm dài **≤ 50 dòng**, file dài **≤ 600 dòng**. Không để lại code chết hay code comment-out. Không thêm tính năng ngoài kế hoạch.
- Log chỉ ghi lỗi và sự kiện quan trọng. Logger tự viết khoảng 80 dòng, không dùng spdlog hay fmt (đã có `std::format`).

### 4.2 Nhanh và nhẹ
- **Làm lười mọi thứ (lazy)**: hiện thư viện từ DB ngay lập tức, vẽ xong khung đầu tiên rồi mới quét nền. Chỉ nạp ảnh bìa của các ô đang hiện trên màn hình.
- Render bằng MuPDF ra **BGRA**, đưa thẳng vào bitmap của Direct2D, không chuyển đổi trung gian.
- Cache trang dạng LRU, **giới hạn theo MB**. Prefetch 1 trang trước và 1 trang sau. Khi xuống khay thì giải phóng cache và gọi `SetProcessWorkingSetSize`.
- Cờ build Release: `/O2 /GL /Gy /Gw` và `/LTCG /OPT:REF /OPT:ICF`. Có thể thêm PGO ở backlog.

### 4.3 Bền và ít crash
- **Không để exception vượt ranh giới**: WndProc, hàm thread, callback C và hàm export của DLL đều là `noexcept`, bọc try/catch ở biên. Lỗi trong app được trả về qua `Result<T>` (`std::expected`).
- **Mọi lời gọi MuPDF đều bọc `fz_try`/`fz_catch`** bên trong `eb_mupdf.dll`. Ra ngoài DLL chỉ còn mã lỗi.
- **Chặn crash lặp lại (crash-loop guard)**: trước khi mở hoặc quét một file, ghi file marker `data\pending` (ghi nguyên tử, nội dung `thao tác<TAB>hash`). Mở xong thì xóa. Không phụ thuộc DB, nên vẫn chạy được kể cả khi DB hỏng. Nếu lần chạy trước crash giữa chừng thì lần sau **bỏ qua file đó và đánh dấu "lỗi"**, không tự mở lại. App không bao giờ kẹt trong vòng crash.
- **Ghi minidump** qua `SetUnhandledExceptionFilter` + `MiniDumpWriteDump` vào `data\crash\` để debug.
- **Giới hạn mọi đầu vào**: kích thước file, số trang, kích thước ảnh render, độ dài text mỗi trang, giải nén CBZ (chống zip bomb).
- **Ghi file nguyên tử**: ghi ra file tạm rồi dùng `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`. SQLite chạy WAL, `synchronous=NORMAL`, `busy_timeout`.
- **Số thread cố định**: UI, Render, Scan (độ ưu tiên thấp) và TTS. Giao tiếp bằng `PostMessage` hoặc hàng đợi có khóa. **Không chia sẻ state có thể thay đổi giữa các thread.**
- **RAII cho mọi tài nguyên**: HANDLE, HDC/GDI, COM (`winrt::com_ptr`), các đối tượng `fz_*`, `sqlite3*`.
- **Nạp DLL an toàn**: `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | USER_DIRS)` + `AddDllDirectory(<exe>\libs)`, nạp bằng đường dẫn tuyệt đối, kiểm tra `eb_abi_version()`.
- Cờ hardening: `/W4 /WX /permissive- /utf-8 /sdl /guard:cf`, `/CETCOMPAT /DYNAMICBASE /HIGHENTROPYVA`.

---

## 5. Agent chỉ huy, phân công và chọn model

Quy tắc chi tiết nằm ở `CLAUDE.md` (agent chỉ huy) và `.claude/agents/eb-*.md` (các agent con).

### 5.1 Vai trò và model

| Ký hiệu | Vai trò | Ai / agent | Model · effort | Giá tương đối/token* | Làm gì |
|---|---|---|---|---|---|
| **PO** | Chủ dự án | 👤 Anh/chị | — | — | Chốt yêu cầu, cài công cụ, key Azure, nghiệm thu, duyệt thêm thư viện |
| **LEAD** | **Agent chỉ huy** | Phiên chính (`CLAUDE.md`) | **Opus 5.5** · **high** | 40 | Đọc kế hoạch, chia task, thiết kế header/ABI, quyết định, giao việc, tổng hợp kết quả. Sửa nhỏ ≤ 30 dòng thì tự làm |
| **DEV** | Lập trình | `eb-dev` | **Sonnet 5.5** · medium | 20 | Viết code và test cho từng task theo TDD. Chiếm phần lớn khối lượng việc |
| **FIX** | Sửa lỗi | `eb-fixer` | **Sonnet 5.5** · medium | 20 | Lỗi build, link, test, clang-tidy, ASan, fuzz |
| **RUN** | Việc cơ học | `eb-runner` | **Haiku 5.5** · low | **1** | Chạy build/test/đo kích thước/benchmark, commit, đánh trạng thái trong PLAN.md, tải corpus |
| **REV** | Review code | `eb-reviewer` | **Opus 5.5** · medium | 40 | Review diff của mỗi task: crash, đúng chức năng, hiệu năng, phần rườm rà. Chỉ đọc, không sửa |
| **SEC** | Review bảo mật | `eb-security` | **Opus 5.5** · medium | 40 | Chỉ chạy cho task có đụng parser, đường dẫn, DLL, key hoặc network |
| **EXP** | Chuyên gia | `eb-expert` | **Opus 5.5** · medium | 40 | **Chỉ gọi khi đã thất bại 2 lần**: crash khó, race condition, leak |
| **CI** | Tự động hóa | GitHub Actions | — | 0 | Build, test, lint, kiểm tra kích thước file build |

\* Tỉ lệ giá API theo token đầu vào: Haiku 5.5 $0,10 · Sonnet 5.5 $2 · Opus 5.5 $4 cho 1 triệu token. Quota của gói thuê bao cũng tiêu hao theo tỉ lệ tương tự.

### 5.2 Quy trình cho 1 task
1. **LEAD** soạn "task card" ngắn gồm: ID task, các file cần sửa, tiêu chí hoàn thành, lệnh kiểm tra.
2. **DEV** viết test (đỏ) → viết code → chạy `tools/test.ps1` cho xanh.
3. **REV** review diff. **SEC** review thêm nếu task liên quan bảo mật.
4. Còn lỗi CRITICAL/HIGH thì **DEV** sửa, tối đa 2 vòng. Quá 2 vòng thì chuyển **EXP**.
5. **RUN** chạy toàn bộ test, ASan và kiểm tra kích thước → commit `T<id>: …` → đánh ✅.

### 5.3 Tiết kiệm quota và token, làm nhanh
- **GIỚI HẠN MODEL (quy định của PO):** **chỉ phiên chính (LEAD) chạy Opus 5.5 · high.** Mọi agent hay phiên do LEAD gọi **tối đa là Opus 5.5 · medium** (hoặc Sonnet, Haiku). **Không bao giờ dùng xhigh, max hay Fable**, kể cả cho phiên chính.
- **Bậc thang leo model**: Haiku → Sonnet → Opus medium (EXP) → Opus high (LEAD tự xử lý trong phiên chính). Chỉ leo lên bậc trên khi bậc dưới đã thất bại 2 lần.
- **Task card tối giản**: không dán lịch sử hay nội dung file vào, agent tự đọc file cần thiết. Agent chỉ trả về **báo cáo ≤ 15 dòng** theo mẫu cố định.
- **Script build/test chạy "im lặng"** (T0.11): chỉ in lỗi và 1 dòng tổng kết. Đây là chỗ tiết kiệm token lớn nhất.
- **LEAD không đọc file lớn** và chỉ Grep đúng đoạn cần trong PLAN.md. File lớn để agent con đọc, giúp context của LEAD gọn.
- **Chạy song song** tối đa 3 task độc lập, sửa các file khác nhau, mỗi task trong một worktree riêng. Ví dụ ở Phase 1, T1.1–T1.3 chạy song song với spike Piper T0.8.
- Prompt cho agent viết bằng **tiếng Anh**, vì tiếng Việt tốn nhiều token hơn. LEAD vẫn trả lời anh/chị bằng tiếng Việt.
- **Mỗi phase dùng 1 phiên mới.** Cuối phase chạy `/ecc:save-session` rồi mở phiên mới để context luôn nhỏ.
- `CLAUDE.md` và các file agent được **giữ ổn định** để tận dụng prompt cache.
- Việc nhỏ (≤ 30 dòng) thì LEAD tự làm, không tạo agent con, vì mỗi agent mới phải nạp lại context từ đầu.

### 5.4 Cách chạy
- Mở Claude Code trong `D:\eBook Reader` và chọn model **Opus** cho phiên chính (`/model`). `CLAUDE.md` được nạp tự động, phiên đó chính là agent chỉ huy.
- Các agent `eb-*` được nạp tự động từ `.claude/agents/`.
- Phiên chính chạy **Opus 5.5 với effort high**. Chọn trong `/model`, phần chỉnh effort.
- Gói hiện tại **không có Fable**. `eb-expert`, `eb-reviewer` và `eb-security` dùng **Opus 5.5 · medium**.
- LEAD gọi agent con bằng công cụ Agent. Agent con chạy nền và tự báo kết quả về LEAD. Agent con **không** gọi được agent khác, nên mọi điều phối đều nằm ở LEAD.
- File agent mới chỉ được nạp khi phiên khởi động. Sau khi sửa `.claude/agents/` thì mở lại bằng `claude --continue` (giữ nguyên cuộc trò chuyện).

### 5.5 Commit local (chống mất code)
- Repo public: **github.com/tiepcons/ebook-reader**. Commit và CI (`tools/ci.ps1`) đều chạy **local**. **Chỉ push `main` khi xong trọn một phase** và CI local xanh (quyết định của PO ngày 2026-10-23).
- **Mỗi task xong (✅) có 1 commit** `T<id>: …` do RUN thực hiện.
- **Commit checkpoint** `wip(T<id>): …` trước mỗi lần giao việc cho agent sửa code, trước thao tác rủi ro, và cuối mỗi phiên. Luôn quay lại được bằng `git restore` hoặc `git reset`.
- Task chạy song song dùng worktree trên nhánh `task/T<id>`. LEAD merge về `main` sau khi review đạt.
- Chống mất ổ đĩa: việc push cuối mỗi phase lên GitHub cũng là bản sao lưu.

---

## 6. Danh sách task

ƯL = ước lượng, tính theo ngày công.

### PHASE 0: Khởi tạo và spike (2–3 ngày)

| ID | Task | Phụ trách | Phụ thuộc | ƯL | Tiêu chí hoàn thành | TT |
|---|---|---|---|---|---|---|
| T0.1 | Cài **VS 2022 Community** (workload *Desktop development with C++*, tick thêm *C++ Clang tools*, đã kèm CMake và Ninja) và **Git** | **PO** | — | 0,3 | `cl`, `cmake`, `ninja`, `clang-cl` đều chạy được | ✅ |
| T0.2 | Cài giọng tiếng Việt cho Windows: Settings → Time & Language → Language → Vietnamese → tick Speech | **PO** | — | 0,1 | Thấy giọng "Microsoft An" | ✅ |
| T0.3 | `git init`, `.gitignore`, `.clang-format`, `.clang-tidy`, `.editorconfig`, LICENSE AGPL-3.0, README | DEV | — | 0,2 | Có commit đầu | ✅ |
| T0.4 | CMake + Presets `debug`/`release`/`asan`/`fuzz`. Các cờ ở mục 4. Manifest có PerMonitorV2, longPathAware, ComCtl v6 | DEV | T0.1 | 0,4 | Cửa sổ Win32 trống build sạch. Exe Release ≤ 200 KB | ✅ 101 KB (preset fuzz chuyển sang T8.3) |
| T0.5 | Vendor MuPDF (submodule, ghim tag), build **`eb_mupdf.dll`** với `/MT` và **không có font CJK** (`TOFU_CJK`) | DEV | T0.4 | 0,8 | DLL ≤ 10 MB | ✅ 7,9 MB |
| T0.6 | **Spike định dạng**: mở EPUB, PDF, MOBI, **AZW3**, FB2, CBZ, và TXT→HTML qua MuPDF | DEV | T0.5 | 0,4 | Có báo cáo. Nếu AZW3 không được thì kích hoạt T1.9 | ✅ (AZW3 ✗, xem D16) |
| T0.7 | Vendor SQLite amalgamation và doctest | DEV | T0.4 | 0,1 | | ✅ |
| T0.8 | **Spike Piper**: onnxruntime (bản prebuilt từ GitHub release) + espeak-ng, đọc thử `vi_VN` và `en_US` | DEV | T0.4 | 0,5 | Ra được file wav tiếng Việt | ✅ RTF ~0,1 (D19) |
| T0.9 | CI: build Debug/Release, chạy test, clang-tidy, **cảnh báo nếu exe hoặc DLL vượt ngưỡng kích thước** | DEV | T0.5 | 0,4 | **CI chạy local** (`tools/ci.ps1`: debug, asan, lint, release, kiểm tra kích thước), theo yêu cầu của PO, không dùng GitHub Actions | ⬜ |
| T0.10 | Corpus test: Project Gutenberg, Standard Ebooks, truyện tiếng Việt public domain. Mỗi định dạng ít nhất 3 file, cộng 5 file cố tình làm hỏng | RUN + **PO** | — | 0,3 | Có `tests/corpus/` | ✅ 8 MB |
| T0.11 | Script **im lặng** `tools/build.ps1` và `tools/test.ps1` (tham số preset): chỉ in lỗi đầu tiên của mỗi file, số test pass/fail và kích thước exe/DLL | LEAD | T0.4 | 0,2 | Build thành công chỉ in 1 dòng | ✅ |

### PHASE 1: Nền tảng và engine đọc (4 ngày)

| ID | Task | Phụ trách | Phụ thuộc | ƯL | Tiêu chí hoàn thành | TT |
|---|---|---|---|---|---|---|
| T1.1 | `base/`: `Result`, `Log` (khoảng 80 dòng), `Handle` RAII, `Text` (chuyển UTF-8/16, **bỏ dấu tiếng Việt** để tìm kiếm) | DEV | T0.4 | 0,5 | Có unit test | ✅ |
| T1.2 | `Paths` (portable, có fallback), `AtomicFile`, `Hash` (BCrypt) | DEV | T1.1 | 0,4 | Test thư mục chỉ đọc, đổi tên file thì hash không đổi | ✅ |
| T1.3 | `CrashGuard`: minidump và crash-loop guard (file marker `data\pending`) | DEV | T1.2 | 0,4 | Test: giả lập crash thì lần sau bỏ qua file đó | ✅ |
| T1.4 | `DllLoader` an toàn và kiểm tra `eb_abi_version` | DEV + SEC | T1.1 | 0,3 | Thiếu DLL hoặc sai phiên bản thì báo lỗi rõ ràng, không crash | ✅ |
| T1.5 | **C ABI `eb_mupdf.h`**: open (path hoặc buffer + magic), close, layout(w, h, em, user_css), page_count, render_bgra(scale, tint), page_text (kèm vị trí ký tự), toc, meta, cover, location↔page. Có thêm hàm free | DEV | T0.6 | 0,6 | Header được REV duyệt | ✅ |
| T1.6 | Viết phần cài đặt `eb_mupdf.c`: mỗi thread một context (`fz_clone_context` + lock), **bọc `fz_try` 100%**, giới hạn tài nguyên | DEV | T1.5 | 1,2 | Mở 100% file tốt trong corpus, 0 crash với file hỏng | ✅ |
| T1.7 | `TxtToFb2` (đổi thiết kế: TXT → FB2 trong bộ nhớ, vì HTML handler đã tắt): tự đoán encoding (UTF-8 có/không BOM, UTF-16, Windows-1258), escape ký tự, tách đoạn thành `<p>`, tách chương theo tiêu đề kiểu "Chương N" | DEV | T1.6 | 0,4 | TXT tiếng Việt hiển thị đúng dấu, có mục lục | ✅ |
| T1.8 | REV + SEC review Phase 1 | REV, SEC | T1.1–T1.7 | 0,2 | Không còn lỗi HIGH | ✅ |
| T1.9 | *(Chỉ khi cần)* Chuyển AZW3 sang EPUB bằng libmobi (LGPL), cần PO duyệt thêm thư viện | DEV | T0.6 | 1 | | ✅ |

### PHASE 2: Thư viện và ảnh bìa (3–4 ngày)

| ID | Task | Phụ trách | Phụ thuộc | ƯL | Tiêu chí hoàn thành | TT |
|---|---|---|---|---|---|---|
| T2.1 | Schema và migration: `books` (id = hash nội dung theo D8, path, folder, title, author, title_nd/author_nd đã bỏ dấu, lang, fmt, size, mtime, status), `progress`, `bookmarks`, `tts_usage`, `kv` (cài đặt). Không có bảng `pending` (dùng file marker, xem T1.3) | DEV | T1.2 | 0,5 | Có test migration | ⬜ |
| T2.2 | `Scanner`: quét **đệ quy**, lọc phần mở rộng, chạy trên thread ưu tiên thấp, **quét tăng dần** (so mtime và size), khớp file di chuyển qua hash, file mất thì đặt `missing` | DEV | T2.1, T1.3 | 1 | Quét 1000 sách lần đầu ≤ 20 giây, quét lại ≤ 1 giây, UI không đơ | ⬜ |
| T2.3 | `Metadata`: lấy tên sách, tác giả, ngôn ngữ qua `eb_mupdf`. Không có thì lấy theo tên file | DEV | T1.6 | 0,4 | Test từng định dạng | ⬜ |
| T2.4 | `CoverCache`: lấy ảnh bìa nhúng trong sách hoặc render trang 1, thu nhỏ còn 320px (cạnh dài), mã hóa JPEG q85 bằng **WIC**. TXT thì dùng bìa mặc định | DEV | T2.3 | 0,6 | 100% sách trong corpus có ảnh bìa | ⬜ |
| T2.5 | REV + SEC review (path traversal, symlink, zip bomb) | REV, SEC | T2.1–T2.4 | 0,2 | | ⬜ |

### PHASE 3: Khung Win32 và giao diện thư viện (4–5 ngày)

| ID | Task | Phụ trách | Phụ thuộc | ƯL | Tiêu chí hoàn thành | TT |
|---|---|---|---|---|---|---|
| T3.1 | `App`: WinMain, vòng lặp message, khởi tạo COM/WinRT, CrashGuard, xử lý DPI theo từng màn hình | DEV | T1.3 | 0,5 | Khởi động ≤ 0,3 giây | ⬜ |
| T3.2 | `MainWnd`: bố cục có splitter, thanh trên gồm ô tìm kiếm, sắp xếp và nút "Quét lại", "Mở thư mục ebooks" | DEV | T3.1 | 0,6 | | ⬜ |
| T3.3 | `LibraryPane` phần **cây thư mục**: TreeView dựng từ cấu trúc thư mục con của `ebooks\` | DEV | T2.2 | 0,5 | Bấm thư mục nào thì lọc sách trong thư mục đó | ⬜ |
| T3.4 | `LibraryPane` phần **lưới ảnh bìa**: ListView ảo (`LVS_OWNERDATA`), ảnh bìa nạp bất đồng bộ, chỉ nạp các ô đang hiện | DEV | T2.4 | 1 | Cuộn mượt với 5000 sách, RAM ≤ 40 MB | ⬜ |
| T3.5 | Tìm kiếm không phân biệt dấu ("kim dung" khớp "Kim Dung"), sắp xếp, bộ lọc **"Đang đọc dở"** có hiện % | DEV | T2.1 | 0,5 | Kết quả ≤ 30 ms | ⬜ |
| T3.6 | Menu chuột phải (Mở, Mở thư mục chứa, Xóa khỏi thư viện), quét lại khi cửa sổ được focus | DEV | T3.4 | 0,3 | | ⬜ |
| T3.7 | REV review và **PO nghiệm thu**: thả 100 sách nhiều tầng thư mục | REV, **PO** | T3.1–T3.6 | 0,3 | | ⬜ |

### PHASE 4: Đọc sách, vị trí đọc, theme, font (6–7 ngày)

| ID | Task | Phụ trách | Phụ thuộc | ƯL | Tiêu chí hoàn thành | TT |
|---|---|---|---|---|---|---|
| T4.1 | `ReaderPane`: vẽ trang bằng Direct2D, chế độ phân trang và cuộn, zoom cho PDF/CBZ | DEV | T1.6, T3.1 | 1,2 | | ✅ |
| T4.2 | `RenderWorker`: 1 thread, prefetch ±1 trang, LRU giới hạn MB, hủy job đã cũ khi người dùng lật nhanh | DEV | T4.1 | 0,7 | Lật trang ≤ 30 ms | ✅ |
| T4.3 | `Location` bền vững. Sách reflow: (chapter, vị trí ký tự), không dùng số trang. PDF/CBZ: (page, cuộn %, zoom) | DEV | T1.5 | 0,4 | Đổi cỡ chữ vẫn về đúng đoạn | ✅ |
| T4.4 | **`ProgressStore`**: lưu Location, %, thời điểm và câu TTS theo hash. Ghi khi lật trang (debounce 1 giây), đóng sách, xuống khay, thoát và mỗi 30 giây | DEV | T2.1, T4.3 | 0,6 | Kill process thì lệch ≤ 1 trang | ✅ |
| T4.5 | **Khôi phục vị trí**: mở sách thì nhảy về chỗ cũ, hiện thanh nhỏ "Tiếp tục từ … Y% · [Về đầu]". Tùy chọn mở lại sách cuối cùng khi khởi động (có crash-loop guard bảo vệ) | DEV | T4.4 | 0,3 | | ✅ |
| T4.6 | **Theme**: preset **Trắng** (#FFFFFF/#1A1A1A), **Đen** (#121212/#D0D0D0), **Giấy sách** (#F4ECD8/#3B2F1E), **Tự chỉnh** màu nền, màu chữ, màu link, màu tô sáng TTS qua `ChooseColor`. Mọi định dạng đổi màu qua `fz_tint_pixmap` (render opts tint=1 + fg/bg); user CSS của reflow cố định (nền trong suốt) nên đổi theme không bố trí lại sách. Ảnh trong sách cũng nhuộm theo theme; tùy chọn bỏ qua ảnh → Backlog (hiện chỉ có cho CBZ). Thanh tiêu đề chuyển tối theo theme | DEV | T4.1 | 0,8 | Đổi theme ≤ 100 ms, không cần mở lại sách | ✅ |
| T4.7 | **FontManager**, 3 chế độ: **Theo sách** (font nhúng + CSS gốc) / **Ghi đè font** / **Ghi đè hết** (font, cỡ chữ, giãn dòng, lề). Kèm sẵn **Literata** và **Be Vietnam Pro** (OFL). Tự nhận font trong `fonts\`. Dùng thêm font hệ thống (Georgia, Cambria, Segoe UI, Times New Roman). Dự phòng bằng Noto khi font sách thiếu dấu tiếng Việt | DEV | T1.6 | 1 | Sách có font thiếu dấu vẫn hiển thị đúng | ✅ |
| T4.8 | Mục lục (TreeView bên cạnh), bookmark đơn giản, nhảy đến %, thanh tiến độ | DEV | T4.1 | 0,6 | | ✅ |
| T4.9 | Phím tắt (←/→, PgUp/PgDn, Space, F11, Ctrl+B, Ctrl+±), toàn màn hình | DEV | T4.1 | 0,2 | | ✅ |
| T4.10 | REV review, đo hiệu năng và **PO nghiệm thu** (đọc dở 3 truyện, đổi theme và font, thoát rồi mở lại) | REV, RUN, **PO** | T4.1–T4.9 | 0,3 | RAM khi đọc ≤ 120 MB | ⬜ |

### PHASE 5: Khay hệ thống và hộp thoại khi đóng (1,5–2 ngày)

| ID | Task | Phụ trách | Phụ thuộc | ƯL | Tiêu chí hoàn thành | TT |
|---|---|---|---|---|---|---|
| T5.1 | `Tray`: `Shell_NotifyIcon`, tooltip, menu (Mở, ▶/⏸ Đọc, ⏭ Câu tiếp, ⏹ Dừng, Thoát). Double-click để mở. Tự đăng ký lại icon khi Explorer khởi động lại (`TaskbarCreated`) | DEV | T3.1 | 0,5 | | ⬜ |
| T5.2 | **`CloseAsk`**: bấm X thì hiện `TaskDialogIndirect` với 3 nút **[Thoát hẳn] [Xuống khay] [Hủy]** và ô ☐ *Ghi nhớ lựa chọn* (verification checkbox). Đổi lại được trong Settings | DEV | T5.1 | 0,3 | Cả 3 lựa chọn và ô ghi nhớ đều đúng | ⬜ |
| T5.3 | Xuống khay: lưu tiến độ, giải phóng cache và Direct2D, giảm working set. **TTS vẫn đọc tiếp** | DEV | T5.2, T4.4 | 0,3 | RAM ở khay ≤ 20 MB | ⬜ |
| T5.4 | Chỉ chạy 1 instance: named mutex, gửi đường dẫn file sang instance đang chạy bằng `WM_COPYDATA` | DEV | T3.1 | 0,3 | | ⬜ |
| T5.5 | REV review và **PO nghiệm thu** | REV, **PO** | T5.1–T5.4 | 0,2 | | ⬜ |

### PHASE 6: TTS tiếng Anh/Việt bằng WinRT và Piper (5–6 ngày)

| ID | Task | Phụ trách | Phụ thuộc | ƯL | Tiêu chí hoàn thành | TT |
|---|---|---|---|---|---|---|
| T6.1 | `ITtsEngine`: liệt kê giọng, đọc, tạm dừng, đọc tiếp, dừng, tốc độ, âm lượng, callback (bắt đầu câu, từng từ, kết thúc). Mọi callback chuyển về thread UI bằng `PostMessage` | DEV | T1.1 | 0,3 | | ⬜ |
| T6.2 | `Segmenter`: tách câu Anh và Việt, xử lý chữ viết tắt và số, bỏ header/footer và số trang của PDF | DEV | T1.6 | 0,7 | ≥ 30 test case | ⬜ |
| T6.3 | `LangDetect`: lấy ngôn ngữ từ metadata. Nếu không có thì đoán theo từng câu dựa vào tỉ lệ ký tự có dấu tiếng Việt. Người dùng chọn cố định được | DEV | T6.2 | 0,3 | Đúng ≥ 95% | ⬜ |
| T6.4 | **`WinRtTts`**: C++/WinRT `SpeechSynthesizer` + `MediaPlayer`, dùng giọng OneCore (Microsoft An, David, Zira...), lấy word boundary để tô sáng. Thiếu giọng Việt thì hướng dẫn cài | DEV | T6.1 | 1,2 | Đọc được cả tiếng Anh và tiếng Việt | ⬜ |
| T6.5 | `TtsController`: tô sáng câu đang đọc, **tự lật trang**, lưu vị trí câu vào ProgressStore, chạy tiếp khi app ở khay | DEV | T6.4, T4.4 | 0,9 | Đọc liên tục hết 1 chương | ⬜ |
| T6.6 | `TtsBar` và trang TTS trong Settings: chọn engine, giọng Anh, giọng Việt, tốc độ, nghe thử | DEV | T6.5 | 0,4 | | ⬜ |
| T6.7 | **`PiperTts`**: chạy `libs\piper\piper.exe` dưới dạng tiến trình con, mỗi giọng một tiến trình, giữ tiến trình sống giữa các câu, dùng `--json-input`, mỗi câu ra một file WAV tạm trong `data\tts-cache\piper\`. Phát bằng `MediaPlayer`, tô sáng theo cả câu. Chỉ bật khi có `libs\piper\` và giọng trong `voices\`. Tổng hợp trước câu tiếp theo | DEV | T0.8, T6.1 | 1,3 | Khoảng lặng giữa 2 câu ≤ 300 ms. Thiếu Piper thì engine này bị ẩn | ✅ |
| T6.8 | REV + SEC review và **PO nghiệm thu** (1 chương tiếng Việt và 1 chương tiếng Anh, có xuống khay giữa chừng) | REV, SEC, **PO** | T6.1–T6.7 | 0,3 | | ⬜ |

### PHASE 7: Azure Speech và bộ đếm % free (3–4 ngày)

**Thiết kế bộ đếm:**
- Free tier F0 là **500.000 ký tự/tháng**, reset vào ngày 1 hằng tháng (giờ UTC).
- Azure không có API nào chỉ cần key mà xem được lượng đã dùng, nên **app tự đếm**. Sau mỗi request trả về HTTP 200, cộng số ký tự tính phí (text gồm khoảng trắng và dấu câu, không tính thẻ SSML) vào `tts_usage(month, chars)`.
- Hiện: *"Đã dùng 123.456 / 500.000 (24,7%) · Còn 75,3% · Reset 01/11 UTC"*.
- Cảnh báo ở mức 80% và 95%. Khi đạt 100% thì làm theo tùy chọn: **tự chuyển sang giọng Windows** (mặc định), hỏi người dùng, hoặc dừng đọc.
- **Audio cache** theo hash(giọng + tốc độ + text): đọc lại không tốn quota. Cache giới hạn 300 MB, khi khởi động thì xóa file cũ nhất.
- Có ô **"Hiệu chỉnh số đã dùng"** để nhập số liệu từ Azure Portal.

| ID | Task | Phụ trách | Phụ thuộc | ƯL | Tiêu chí hoàn thành | TT |
|---|---|---|---|---|---|---|
| T7.1 | **PO tạo Azure Speech gói Free F0** và lấy Key 1 cùng Region | **PO** | — | 0,2 | | ⬜ |
| T7.2 | `SecretStore`: lưu key bằng **Windows Credential Manager** (`CredWriteW`). Không ghi vào DB, cài đặt hay log | DEV + SEC | T1.1 | 0,3 | Grep thư mục `data\` không thấy key | ⬜ |
| T7.3 | Trang Azure trong Settings (file .rc): ô key (ẩn ký tự), region, tier F0/S0, quota/tháng, nút **Kiểm tra kết nối** (`GET voices/list`), chọn giọng Việt (HoaiMy, NamMinh) và giọng Anh | DEV | T7.2 | 0,6 | Key sai thì báo lỗi 401 rõ ràng | ⬜ |
| T7.4 | `AzureTts`: gọi `POST https://{region}.tts.speech.microsoft.com/cognitiveservices/v1` qua **WinHTTP** (bất đồng bộ), dùng SSML, nhận `audio-24khz-48kbitrate-mono-mp3` (MediaPlayer giải mã sẵn, không cần thêm codec). Prefetch 2 câu. Gặp 429/5xx thì thử lại theo backoff, sau đó chuyển sang WinRT | DEV | T6.1, T7.3 | 0,9 | Mất mạng thì tự chuyển engine, không crash | 🟨 |
| T7.5 | **`AzureUsage`**: đếm ký tự, ghi trong transaction, tính %, ngày reset, xử lý sang tháng mới | DEV | T2.1 | 0,4 | Unit test: đếm đúng, đổi tháng đúng, Unicode tiếng Việt đúng | ⬜ |
| T7.6 | Hiện **% còn lại** trong Settings, trên TtsBar ("Azure 75%") và trong menu tray. Cảnh báo và xử lý khi đạt 100% | DEV | T7.5 | 0,4 | | ⬜ |
| T7.7 | Audio cache và chức năng hiệu chỉnh hoặc đặt lại bộ đếm | DEV | T7.4, T7.5 | 0,3 | Đọc lại 1 chương thì bộ đếm không tăng | 🟨 |
| T7.8 | SEC + REV review và **PO nghiệm thu** (so số đếm của app với Azure Portal) | SEC, REV, **PO** | T7.1–T7.7 | 0,3 | Sai lệch < 5% | ⬜ |

### PHASE 8: Củng cố và chống crash (4 ngày, cộng phần làm liên tục trong mọi phase)

| ID | Task | Phụ trách | Phụ thuộc | ƯL | Tiêu chí hoàn thành | TT |
|---|---|---|---|---|---|---|
| T8.1 | Coverage của `src/` (trừ `ui/`) **≥ 80%**, đo bằng OpenCppCoverage | RUN | mọi phase | 0,8 | | ⬜ |
| T8.2 | Integration test: quét corpus → mở → render → lấy ảnh bìa → lấy text → lưu rồi khôi phục vị trí | RUN | P4 | 0,4 | Toàn bộ pass | ⬜ |
| T8.3 | **Fuzz** bằng libFuzzer (clang-cl) cho `eb_mupdf` (mọi định dạng), `TxtToHtml`, `Segmenter` | RUN | P2 | 1 | Mỗi target chạy 1 giờ, 0 crash | ⬜ |
| T8.4 | Chạy toàn bộ test với ASan, **chạy thử liên tục 8 giờ** (soak test: tự lật trang qua 50 sách, xuống khay rồi mở lại) | RUN | T8.2 | 0,5 | 0 crash, RAM không tăng dần (không leak) | ⬜ |
| T8.5 | Phân tích tĩnh: clang-tidy, `/analyze`, cppcheck | RUN, REV | — | 0,3 | 0 cảnh báo | ⬜ |
| T8.6 | **Rà phần rườm rà**: code chết, phụ thuộc thừa, kích thước file build, chuỗi và tài nguyên thừa | REV | — | 0,3 | Đạt mục tiêu kích thước | ⬜ |
| T8.7 | Review toàn bộ bằng `/code-review` effort cao, cộng `security-review` | REV, SEC | T8.1–T8.6 | 0,4 | Không còn lỗi CRITICAL/HIGH | ⬜ |
| T8.8 | Test thủ công theo checklist ở mục 9 | **PO** | T8.7 | 0,5 | | ⬜ |

### PHASE 9: Phát hành (1 ngày)

| ID | Task | Phụ trách | Phụ thuộc | ƯL | Tiêu chí hoàn thành | TT |
|---|---|---|---|---|---|---|
| T9.1 | `tools/package.ps1` tạo **zip portable cơ bản** (exe, `libs\eb_mupdf.dll`, 2 font, thư mục `ebooks\` rỗng, `licenses\`) và **zip gói Piper** riêng | DEV | P8 | 0,3 | Zip cơ bản ≤ 10 MB, chạy được trên máy Windows sạch | ⬜ |
| T9.2 | Tuân thủ license: AGPL (công bố mã nguồn), MuPDF, SQLite, Piper/espeak-ng (GPL), onnxruntime (MIT), font OFL | DEV + SEC | T9.1 | 0,2 | | ⬜ |
| T9.3 | `release.yml`: push tag `v*` thì đăng 2 file zip lên GitHub Releases | DEV → CI | T9.1 | 0,2 | | ⬜ |
| T9.4 | *(Tùy chọn)* Ký số code miễn phí qua SignPath.io Foundation cho OSS | **PO** + DEV | T9.3 | 0,3 | | ⬜ |

---

## 7. Mốc và tổng thời gian

| Mốc | Gồm | Tích lũy | Kết quả |
|---|---|---|---|
| M1 Engine | P0, P1 | khoảng 7 ngày | Mở và render được mọi định dạng (bản demo) |
| M2 Thư viện | P2, P3 | khoảng 15 ngày | Thả sách vào `ebooks\` là thấy ảnh bìa và cây thư mục |
| **M3 MVP** | P4, P5 | khoảng 23 ngày | Đọc sách, nhớ vị trí, theme, font, khay hệ thống |
| M4 TTS | P6, P7 | khoảng 32 ngày | WinRT, Piper, Azure kèm % quota |
| **M5 v1.0** | P8, P9 | **khoảng 37 ngày** | Đã chống crash, test và review đầy đủ, phát hành zip |

## 8. Mục tiêu đo được

| Chỉ số | Mục tiêu |
|---|---|
| Kích thước exe / eb_mupdf.dll / zip cơ bản | ≤ 3 MB / ≤ 10 MB / ≤ 10 MB |
| Khởi động lạnh đến khi thư viện hiện | ≤ 0,3 giây (1000 sách đã index) |
| Quét lần đầu / quét lại 1000 sách | ≤ 20 giây / ≤ 1 giây |
| Mở sách / lật trang | ≤ 0,8 giây / ≤ 30 ms |
| RAM khi xem thư viện / khi đọc / khi ở khay | ≤ 40 / ≤ 120 / ≤ 20 MB |
| Crash: corpus, fuzz 1 giờ, soak test 8 giờ | **0** |
| Coverage logic | ≥ 80% |

## 9. Checklist test thủ công (PO)
- [ ] Thả sách vào `ebooks\` nhiều tầng thư mục → thấy sách sau khi focus lại cửa sổ hoặc bấm Quét lại
- [ ] Đổi tên hoặc di chuyển file → vị trí đọc vẫn còn
- [ ] Mở được EPUB, PDF, MOBI, AZW3, FB2, CBZ, TXT (gồm TXT tiếng Việt). Ảnh bìa đúng
- [ ] Đọc dở → thoát → mở lại → đúng chỗ. Đổi font hoặc cỡ chữ vẫn đúng chỗ
- [ ] Kill app bằng Task Manager → mở lại → lệch ≤ 1 trang
- [ ] File hỏng: báo lỗi, không crash. Mở lại app thì không bị crash lặp lại
- [ ] Theme Trắng, Đen, Giấy sách, Tự chỉnh màu. PDF cũng đổi màu theo
- [ ] Font: Theo sách, Ghi đè font, Ghi đè hết. Thả font .ttf mới vào `fonts\` → thấy trong danh sách
- [ ] Bấm X: hộp thoại hỏi xuất hiện. Kiểm tra cả 3 nút và ô ghi nhớ
- [ ] Ở khay: menu chạy được, TTS vẫn đọc, RAM ≤ 20 MB
- [ ] TTS WinRT tiếng Việt và tiếng Anh. Piper (sau khi giải nén gói). Xóa `libs\piper` thì app vẫn chạy
- [ ] Azure: nhập key, kiểm tra kết nối, % giảm đúng, đọc lại không tốn quota, đạt 100% thì tự chuyển engine
- [ ] Windows 10 và 11. DPI 100/150/200%. Hai màn hình. Đường dẫn có tiếng Việt và dài hơn 260 ký tự

## 10. Backlog: đã CẮT khỏi v1 để gọn (làm sau nếu cần)
Chế độ cuộn liên tục và thanh cuộn khi zoom PDF (v1 chỉ có chế độ phân trang, pan bằng kéo hoặc lăn chuột) · Đọc 2 trang cùng lúc · tìm kiếm trong sách · kéo thả để nhập sách · khởi động cùng Windows · phím media · cài đặt riêng cho từng sách · ghi chú cho bookmark · tải Piper và font ngay trong app · bộ cài Inno Setup · tự cập nhật · CBR, DJVU · encoding TCVN3 · theo dõi thư mục theo thời gian thực · PGO.

## 11. Rủi ro

| Mức | Rủi ro | Giảm thiểu | Task |
|---|---|---|---|
| HIGH | MuPDF không đọc được AZW3/KF8 | Spike T0.6, nếu cần thì dùng libmobi (T1.9, cần PO duyệt) | T0.6 |
| HIGH | Bug trong MuPDF với file độc hại làm crash cả app (vì chạy chung process) | Bọc `fz_try` toàn bộ, giới hạn tài nguyên, fuzz, crash-loop guard. *Nếu vẫn crash nhiều: chuyển render sang process riêng (cần thêm khoảng 3 ngày)* | T1.6, T1.3, T8.3 |
| HIGH | Giọng Microsoft An chỉ có qua WinRT | Dùng WinRT, có hướng dẫn cài giọng, Piper làm dự phòng | T6.4 |
| MEDIUM | Bộ đếm Azure chỉ đếm phía client | Ô hiệu chỉnh. Tier F0 bị chặn khi hết quota chứ không tính tiền | T7.5 |
| MEDIUM | Win32 mất nhiều công cho giao diện hơn | Giữ giao diện tối giản, Settings làm bằng file .rc | P3 |
| MEDIUM | Build MuPDF tùy biến trên MSVC | Ghim theo tag, dùng solution có sẵn của MuPDF cho Windows, cache trên CI | T0.5 |
| LOW | DRM, SmartScreen | Báo lỗi thân thiện. SignPath cho OSS | — |

## 12. Chi phí
Công cụ, thư viện, CI, TTS WinRT và Piper: **$0**. Azure F0: **$0** (500.000 ký tự/tháng). Ký số code: **$0** với SignPath cho OSS, nếu không được duyệt thì khoảng $9,99/tháng với Azure Trusted Signing.

---

## 13. Nhật ký thay đổi
- **v1**: kế hoạch đầu tiên.
- **v2**: chốt mở mã nguồn + MuPDF, thư mục `ebooks\`, vị trí đọc theo hash, Azure kèm % free, phân công vai trò.
- **v3**: **chuyển sang Win32 + Direct2D, bỏ Qt.** Bỏ vcpkg, spdlog, FTS5, WebP, FolderWatcher. TXT đi qua MuPDF. Thêm theme (Trắng, Đen, Giấy sách, Tự chỉnh), hệ thống font (theo sách hoặc ghi đè, font OFL), DLL trong `libs\` dùng C ABI, CRT tĩnh, bộ quy tắc nhanh/gọn/nhẹ/bền, crash-loop guard, minidump, soak test. Phát hành bằng zip. Cắt bớt tính năng phụ vào Backlog. Ước lượng giảm còn khoảng 37 ngày.
- **v4**: thêm **agent chỉ huy** (Opus 5.5, `CLAUDE.md`) và 6 agent con trong `.claude/agents/`: `eb-dev` và `eb-fixer` (Sonnet), `eb-runner` (Haiku), `eb-reviewer` và `eb-security` (Opus), `eb-expert` (Fable, chỉ dùng khi leo thang). Thêm quy tắc tiết kiệm quota và token (mục 5.3) và task T0.11 tạo script build/test im lặng.
- **v4.1**: không có Fable nên `eb-expert` chuyển sang Opus 5.5 · xhigh. Thêm mục 5.5 về commit local. Khởi tạo repo git local.
- **v4.2**: **giới hạn model**: chỉ phiên chính chạy Opus 5.5 · high. Mọi agent con tối đa Opus 5.5 · medium (`eb-reviewer`, `eb-security`, `eb-expert` hạ xuống medium). Cấm dùng xhigh, max và Fable.
