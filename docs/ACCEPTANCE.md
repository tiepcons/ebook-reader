# Nghiệm thu tay (PO) — bản 0.9.0-alpha.10

Tick `[x]` khi đạt. Nếu lỗi, ghi ngắn vào cột **Ghi chú** (đã làm gì → thấy gì). Gửi lại file này là đủ để sửa.
Thời gian dự kiến: khoảng 60–90 phút (Azure thêm khoảng 15 phút).

## 0. Chuẩn bị (5 phút)

1. Bản test đã được cài sẵn trong `D:\EbookReader-Test\` (đủ Piper, VieNeu, DLL; `data\` và `ebooks\` của bạn được giữ nguyên khi cập nhật).
   - Không chạy exe trong `build\`.
2. Chuẩn bị khoảng 100 sách nhiều định dạng: EPUB, PDF, MOBI, AZW3, FB2, CBZ, TXT (có ít nhất 1 TXT tiếng Việt).
   Để trong vài tầng thư mục con, ví dụ `Tiểu thuyết\Việt Nam\...`.
3. Mở Task Manager → tab **Details** → chuột phải tiêu đề cột → thêm cột **Memory (private working set)**. Cột này dùng để đo RAM.
4. Piper đã có sẵn; giọng tiếng Việt đã nằm trong `data\voices\`. Giọng tiếng Anh (tùy chọn) tải theo `README.txt`.

## 1. Thư viện — T3.7

| # | Bước | Kỳ vọng | Đạt | Ghi chú |
|---|---|---|---|---|
| 1.1 | Chép khoảng 100 sách (nhiều tầng thư mục) vào `ebooks\`, rồi mở app | Thư viện hiện đủ sách, có ảnh bìa, cây thư mục đúng tầng | [ ] | |
| 1.2 | Khi app đang mở, chép thêm 3 sách. Bấm sang cửa sổ khác rồi quay lại (hoặc bấm Quét lại) | 3 sách mới xuất hiện | [ ] | |
| 1.3 | Đổi tên 1 cuốn đang đọc dở, hoặc chuyển nó sang thư mục khác, rồi quét lại và mở | Vẫn mở đúng chỗ đọc dở | [ ] | |
| 1.4 | Chép 1 file hỏng vào `ebooks\` (ví dụ đổi đuôi một file .txt thành .epub) | Báo lỗi, không crash. Tắt rồi mở lại app vẫn không crash | [ ] | |

## 2. Đọc sách, theme, font — T4.10

| # | Bước | Kỳ vọng | Đạt | Ghi chú |
|---|---|---|---|---|
| 2.1 | Mở lần lượt 1 sách mỗi định dạng: EPUB, PDF, MOBI, AZW3, FB2, CBZ, TXT tiếng Việt | Mở được tất cả, chữ tiếng Việt đúng dấu | [ ] | |
| 2.2 | Đọc dở 3 truyện, mỗi truyện tới một trang khác nhau. Thoát hẳn rồi mở lại từng cuốn | Mỗi cuốn mở lại đúng trang | [ ] | |
| 2.3 | Đang đọc: đổi cỡ chữ, rồi đổi font | Vẫn ở đúng đoạn đang đọc | [ ] | |
| 2.4 | Đang đọc thì kill app trong Task Manager, sau đó mở lại | Lệch tối đa 1 trang | [ ] | |
| 2.5 | Theme Trắng, Đen, Giấy sách, Tự chỉnh màu. Thử cả với PDF | Màu nền và chữ đổi theo, PDF cũng đổi | [ ] | |
| 2.6 | Font: Theo sách, Ghi đè font, Ghi đè hết. Chép 1 file .ttf mới vào `fonts\` | Font mới xuất hiện trong danh sách | [ ] | |
| 2.7 | Ctrl+T (mục lục), Ctrl+B (đặt/bỏ dấu trang), Ctrl+G (đi đến %) | Hoạt động đúng | [ ] | |
| 2.7b | Mở 1 truyện có trang mục lục trong sách: bấm vào tên chương; rê chuột lên liên kết | Nhảy đúng chương; con trỏ thành bàn tay | [ ] | |
| 2.7c | Ô chọn chương ở thanh trên: chọn 1 chương; lật trang qua chương khác | Nhảy ngay tới chương; ô tự đổi theo chương đang đọc; mũi tên vẫn lật trang | [ ] | |
| 2.7d | Mở 1 truyện EPUB lớn (vd. trong `D:\Truyen`), đọc tới giữa truyện, quay về thư viện rồi mở lại (giữ nguyên cỡ cửa sổ, cỡ chữ). Thoát app, mở lại lần nữa | Lần đầu chậm như cũ; các lần mở lại hiện ngay (< 1 s) đúng trang đang đọc | [ ] | |
| 2.8 | **Đo RAM** khi đang đọc (cột private working set) | **≤ 120 MB**. Ghi lại số đo | [ ] | MB = |
| 2.9 | Đo RAM khi đang xem thư viện | ≤ 40 MB. Ghi lại số đo | [ ] | MB = |

## 3. Khay hệ thống — T5.5

| # | Bước | Kỳ vọng | Đạt | Ghi chú |
|---|---|---|---|---|
| 3.1 | Bấm X | Hộp thoại có 3 nút **Thoát hẳn / Xuống khay / Hủy** và ô *Ghi nhớ lựa chọn* | [ ] | |
| 3.2 | Thử từng nút. Sau đó tick *Ghi nhớ* rồi bấm X lại | Mỗi nút làm đúng việc. Khi đã ghi nhớ thì app không hỏi nữa | [ ] | |
| 3.3 | Đổi lại lựa chọn đã ghi nhớ trong Cài đặt | App hỏi lại khi bấm X | [ ] | |
| 3.4 | Ở khay: menu chuột phải (Mở, Đọc, Câu tiếp, Dừng, Thoát). Double-click icon | Tất cả chạy được; double-click mở lại cửa sổ | [ ] | |
| 3.5 | Đo RAM khi đang ở khay (chờ khoảng 5 giây) | **≤ 20 MB**. Ghi lại số đo | [ ] | MB = |
| 3.6 | Khi app đang chạy, kéo 1 file .epub trong Explorer thả lên `EbookReader.exe` | File mở trong cửa sổ đang chạy, không mở thêm app thứ 2 | [ ] | |

## 4. Đọc to (TTS) — T6.8

| # | Bước | Kỳ vọng | Đạt | Ghi chú |
|---|---|---|---|---|
| 4.1 | Giọng Windows: đọc 1 chương tiếng Việt | Đọc được, câu đang đọc được tô sáng, tự lật trang | [ ] | |
| 4.2 | Giọng Windows: đọc 1 chương tiếng Anh | Như trên | [ ] | |
| 4.3 | Đang đọc thì đưa app xuống khay | Vẫn đọc tiếp. Dừng/tiếp được từ menu khay | [ ] | |
| 4.4 | Piper (nếu đã giải nén gói và tải giọng): đọc tiếng Việt | Đọc được | [ ] | |
| 4.5 | Xóa hoặc đổi tên `libs\piper` rồi mở app | App vẫn chạy, chỉ không còn Piper trong danh sách | [ ] | |
| 4.6 | VieNeu: model + 25 giọng đã cài sẵn trong `data\vieneu\`. Mở app (chờ vài giây để app kiểm tra file), chọn engine VieNeu, đọc 1 chương có tiêu đề `Chương …:` | Nghe rõ tiêu đề chương, đọc liên tục, câu được tô sáng; đổi tốc độ được | [ ] | |

## 5. Azure — T7.1 + T7.8

**T7.1 — tạo Azure Speech Free F0 (miễn phí 500.000 ký tự/tháng):**
1. Vào https://portal.azure.com → **Create a resource** → tìm **Speech** → **Create**.
2. **Pricing tier: Free F0**. Region nên chọn **Southeast Asia** cho gần.
3. Sau khi tạo xong → **Keys and Endpoint** → chép **KEY 1** và **Location/Region** (ví dụ `southeastasia`).
4. **Không gửi key cho ai**, kể cả dán vào chat. Key chỉ nhập trực tiếp vào app.

| # | Bước | Kỳ vọng | Đạt | Ghi chú |
|---|---|---|---|---|
| 5.1 | Cài đặt giọng đọc → Azure: nhập Key và Region → **Kiểm tra kết nối** | Báo kết nối OK | [ ] | |
| 5.2 | Chọn engine Azure, đọc khoảng 1 chương | Đọc được; "Azure N%" trên thanh TTS giảm dần | [ ] | |
| 5.3 | Đọc lại đúng đoạn vừa đọc | Không tốn thêm quota (đọc từ cache) | [ ] | |
| 5.4 | Sáng hôm sau, so **số ký tự app đếm** với Azure Portal → resource → **Metrics** → *Synthesized Characters* | **Lệch < 5%**. Ghi lại cả 2 số | [ ] | App = / Portal = |
| 5.5 | Trong Cài đặt, nhập số đã dùng gần 100% để giả lập hết quota, rồi đọc tiếp | App tự chuyển sang giọng Windows, có thông báo | [ ] | |

## 6. Môi trường — T8.8 (làm phần nào có máy thì làm phần đó)

| # | Bước | Đạt | Ghi chú |
|---|---|---|---|
| 6.1 | Windows 11 (máy này) | [ ] | |
| 6.2 | Windows 10 (nếu có máy) | [ ] | |
| 6.3 | DPI 100%, 150%, 200% (Cài đặt → Hiển thị → Tỷ lệ): giao diện không vỡ, chữ nét | [ ] | |
| 6.4 | Hai màn hình: kéo cửa sổ qua lại giữa màn khác DPI | [ ] | |
| 6.5 | Đặt app trong thư mục có tên tiếng Việt và đường dẫn dài hơn 260 ký tự | [ ] | |

## Kết quả

- Ngày test: ……
- Máy / Windows: ……
- Lỗi nghiêm trọng (crash, mất vị trí đọc, mất dữ liệu): ……
- Góp ý giao diện / trải nghiệm: ……
