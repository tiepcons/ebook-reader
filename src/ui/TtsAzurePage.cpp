#include "TtsAzurePage.h"

#include "base/SecretStore.h"
#include "base/Text.h"
#include "resource.h"
#include "tts/azure/AzureMessages.h"

#include <algorithm>
#include <array>
#include <utility>

namespace eb {
namespace {

constexpr int kRegionChars = 32, kCountChars = 16;
constexpr const wchar_t* kTierNames[] = {L"F0", L"S0"};

std::wstring text_of(HWND dlg, int id)
{
    wchar_t buf[64]{};
    GetDlgItemTextW(dlg, id, buf, static_cast<int>(std::size(buf)));
    return buf;
}

void warn(HWND dlg, const std::wstring& text)
{
    MessageBoxW(dlg, text.c_str(), L"Azure", MB_OK | MB_ICONWARNING);
}

void set_text(HWND dlg, int id, const std::wstring& text)
{
    SetDlgItemTextW(dlg, id, text.c_str());
}

}

void AzurePage::init(HWND dlg, TtsDialogApi& api)
{
    api_ = &api;
    SendDlgItemMessageW(dlg, IDC_AZ_KEY, EM_LIMITTEXT, kMaxSecretChars, 0);
    SendDlgItemMessageW(dlg, IDC_AZ_REGION, EM_LIMITTEXT, kRegionChars, 0);
    SendDlgItemMessageW(dlg, IDC_AZ_QUOTA, EM_LIMITTEXT, kCountChars, 0);
    SendDlgItemMessageW(dlg, IDC_AZ_USED, EM_LIMITTEXT, kCountChars, 0);
    for (const wchar_t* name : kTierNames)
        SendDlgItemMessageW(dlg, IDC_AZ_TIER, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    show_view(dlg);
}

void AzurePage::show_view(HWND dlg)
{
    const AzureView v = api_->azure_view();
    set_text(dlg, IDC_AZ_REGION, utf8_to_wide(v.region).value_or(std::wstring{}));
    SendDlgItemMessageW(dlg, IDC_AZ_TIER, CB_SETCURSEL, v.tier == AzureTier::S0 ? 1 : 0, 0);
    set_text(dlg, IDC_AZ_QUOTA, std::to_wstring(v.usage.quota));
    used_text_ = std::to_wstring(v.usage.used);
    set_text(dlg, IDC_AZ_USED, used_text_);
    set_text(dlg, IDC_AZ_USAGE, v.usage_ok ? format_status_vi(v.usage) + L" · " + bar_quota_text(v.usage) : L"Chưa đọc được bộ đếm.");
    show_key_state(dlg);
}

void AzurePage::show_key_state(HWND dlg)
{
    const bool has = secret_get(kAzureSecretTarget).has_value();
    set_text(dlg, IDC_AZ_KEYSTATE, has ? L"Đã lưu khóa (để trống để giữ nguyên)." : L"Chưa có khóa.");
    EnableWindow(GetDlgItem(dlg, IDC_AZ_DELETE), has ? TRUE : FALSE);
}

void AzurePage::delete_key(HWND dlg)
{
    if (MessageBoxW(dlg, L"Xoá khóa Azure khỏi máy này?", L"Azure", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    if (!secret_delete(kAzureSecretTarget))
        warn(dlg, L"Không xoá được khóa.");
    key_changed_ = true;
    SetDlgItemTextW(dlg, IDC_AZ_KEY, L"");
    show_key_state(dlg);
}

void AzurePage::reset_counter(HWND dlg)
{
    if (MessageBoxW(dlg, L"Đặt bộ đếm ký tự tháng này về 0?", L"Azure", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    if (!api_->azure_reset_used())
        warn(dlg, L"Không đặt lại được bộ đếm.");
    show_view(dlg);
}

void AzurePage::test_connection(HWND dlg)
{
    const auto region = parse_region(text_of(dlg, IDC_AZ_REGION));
    if (!region || region->empty()) {
        set_text(dlg, IDC_AZ_RESULT, L"Nhập vùng hợp lệ trước, ví dụ southeastasia.");
        return;
    }
    SecretString key = SecretString::from_window_text(GetDlgItem(dlg, IDC_AZ_KEY));
    wipe_key(dlg);
    if (key.view().empty()) {
        auto stored = secret_get(kAzureSecretTarget);
        if (!stored) {
            set_text(dlg, IDC_AZ_RESULT, L"Chưa có khóa để kiểm tra.");
            return;
        }
        key = std::move(*stored);
    } else if (const auto ok = validate_api_key(key); !ok) {
        set_text(dlg, IDC_AZ_RESULT, ok.error().detail);
        return;
    }
    const bool started = probe_.start(*region, std::move(key), [dlg](int status) {
        PostMessageW(dlg, WM_APP_AZURE_PROBE, static_cast<WPARAM>(status), 0);
    });
    set_text(dlg, IDC_AZ_RESULT, started ? L"Đang kiểm tra kết nối…" : L"Đang kiểm tra, vui lòng chờ.");
    EnableWindow(GetDlgItem(dlg, IDC_AZ_TEST), started ? FALSE : TRUE);
}

void AzurePage::on_probe(HWND dlg, int status)
{
    set_text(dlg, IDC_AZ_RESULT, probe_text(status));
    EnableWindow(GetDlgItem(dlg, IDC_AZ_TEST), TRUE);
}

bool AzurePage::on_command(HWND dlg, int id, int code)
{
    if (code != BN_CLICKED)
        return false;
    if (id == IDC_AZ_DELETE)
        delete_key(dlg);
    else if (id == IDC_AZ_RESET)
        reset_counter(dlg);
    else if (id == IDC_AZ_TEST)
        test_connection(dlg);
    else
        return false;
    return true;
}

void AzurePage::wipe_key(HWND dlg) noexcept
{
    HWND edit = GetDlgItem(dlg, IDC_AZ_KEY);
    const int len = std::clamp(GetWindowTextLengthW(edit), 0, static_cast<int>(kMaxSecretChars));
    if (len > 0) {
        std::array<wchar_t, kMaxSecretChars + 1> fill{};
        std::fill_n(fill.begin(), len, L'*');
        SetWindowTextW(edit, fill.data()); // same length: the edit rewrites its buffer in place
    }
    SetWindowTextW(edit, L"");
}

bool AzurePage::store_key(HWND dlg)
{
    const SecretString key = SecretString::from_window_text(GetDlgItem(dlg, IDC_AZ_KEY));
    wipe_key(dlg);
    if (key.view().empty())
        return true;
    if (const auto ok = validate_api_key(key); !ok) {
        warn(dlg, ok.error().detail);
        SetFocus(GetDlgItem(dlg, IDC_AZ_KEY));
        return false;
    }
    // Privacy notice required by the SignPath Foundation terms before any data leaves the machine.
    if (MessageBoxW(dlg,
            L"Khi dùng Azure Speech, đoạn văn bản đang đọc và khóa API của bạn được gửi qua HTTPS tới máy chủ Microsoft "
            L"(<vùng>.tts.speech.microsoft.com), theo Chính sách quyền riêng tư của Microsoft "
            L"(https://privacy.microsoft.com/privacystatement). Ebook Reader không gửi dữ liệu đi đâu khác.\n\n"
            L"Bạn có thể chuyển về giọng đọc offline hoặc xoá khóa bất cứ lúc nào.\n\nĐồng ý lưu khóa và dùng Azure?",
            L"Quyền riêng tư - Azure Speech", MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2) != IDYES)
        return false;
    if (!secret_set(kAzureSecretTarget, key)) {
        warn(dlg, L"Không lưu được khóa vào Windows Credential Manager.");
        return false;
    }
    key_changed_ = true;
    return true;
}

bool AzurePage::read_edits(HWND dlg, AzureEdits& out)
{
    const auto region = parse_region(text_of(dlg, IDC_AZ_REGION));
    const auto quota = parse_char_count(text_of(dlg, IDC_AZ_QUOTA), 1, kMaxAzureQuota);
    const std::wstring used_now = text_of(dlg, IDC_AZ_USED);
    const auto used = parse_char_count(used_now, 0, kMaxAzureQuota);
    if (!region) {
        warn(dlg, L"Vùng không hợp lệ: 3 đến 32 ký tự a-z hoặc 0-9, ví dụ southeastasia.");
    } else if (!quota) {
        warn(dlg, L"Hạn mức tháng phải là số ký tự từ 1 đến 1.000.000.000.000.");
    } else if (!used) {
        warn(dlg, L"Số ký tự đã dùng phải là một số từ 0 trở lên.");
    } else {
        out.region = *region;
        out.quota = *quota;
        out.tier = SendDlgItemMessageW(dlg, IDC_AZ_TIER, CB_GETCURSEL, 0, 0) == 1 ? AzureTier::S0 : AzureTier::F0;
        if (used_now != used_text_)
            out.used = *used;
        return true;
    }
    return false;
}

bool AzurePage::commit(HWND dlg)
{
    AzureEdits e;
    if (!read_edits(dlg, e) || !store_key(dlg))
        return false;
    e.key_changed = key_changed_;
    if (!api_->azure_save(e)) {
        warn(dlg, L"Không lưu được cài đặt Azure.");
        return false;
    }
    return true;
}

}
