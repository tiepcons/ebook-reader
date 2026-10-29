#include "TtsSettings.h"

#include "TtsAzurePage.h"
#include "TtsBar.h"
#include "TtsController.h"
#include "base/Log.h"
#include "resource.h"
#include "tts/azure/AzureMessages.h"

#include <algorithm>
#include <array>
#include <string>

namespace eb {
namespace {

constexpr UINT_PTR kVoicePollTimer = 1;
constexpr UINT kVoicePollMs = 1000;
constexpr int kMaxVoicePolls = 20;
constexpr std::array<const wchar_t*, kTtsKinds> kEngineNames{L"Giọng Windows (có sẵn)", L"Piper (ngoại tuyến)", L"Azure (trực tuyến)", L"VieNeu (ngoại tuyến, tự nhiên hơn)"};
constexpr std::array<int, 2> kVoiceCombos{IDC_TTS_VOICE_EN, IDC_TTS_VOICE_VI};
constexpr std::array<int, 2> kPreviewButtons{IDC_TTS_PREVIEW_EN, IDC_TTS_PREVIEW_VI};

struct DlgState {
    TtsPrefs prefs;
    TtsDialogApi* api = nullptr;
    AzurePage azure;
    std::vector<TtsKind> kinds;
    std::array<std::vector<std::wstring>, 2> ids; // voice id behind each item of the voice combos
    int polls = 0;
    bool loading = true;
    bool accepted = false;
};

TtsKind selected_kind(HWND dlg, const DlgState& st) noexcept
{
    const LRESULT i = SendDlgItemMessageW(dlg, IDC_TTS_ENGINE, CB_GETCURSEL, 0, 0);
    return i >= 0 && static_cast<std::size_t>(i) < st.kinds.size() ? st.kinds[static_cast<std::size_t>(i)] : st.prefs.engine;
}

void add_item(HWND box, const std::wstring& text)
{
    SendMessageW(box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

void fill_voice_combo(HWND dlg, DlgState& st, std::size_t lang, const std::vector<Voice>& all)
{
    HWND box = GetDlgItem(dlg, kVoiceCombos[lang]);
    SendMessageW(box, CB_RESETCONTENT, 0, 0);
    auto& ids = st.ids[lang];
    ids.assign(1, std::wstring{});
    add_item(box, L"(Tự chọn)");
    for (const Voice& v : all) {
        if (voice_has_lang(v, lang == 1 ? Lang::Vi : Lang::En)) {
            ids.push_back(v.id);
            add_item(box, v.name.empty() ? v.id : v.name);
        }
    }
    const std::wstring& want = st.prefs.voice[kind_index(selected_kind(dlg, st))][lang];
    const auto at = std::ranges::find(ids, want);
    SendMessageW(box, CB_SETCURSEL, at == ids.end() ? 0 : static_cast<WPARAM>(at - ids.begin()), 0);
    EnableWindow(GetDlgItem(dlg, kPreviewButtons[lang]), ids.size() > 1 ? TRUE : FALSE);
}

void fill_voices(HWND dlg, DlgState& st)
{
    const TtsKind k = selected_kind(dlg, st);
    const std::vector<Voice> all = st.api->voices(k);
    for (std::size_t lang = 0; lang < 2; ++lang)
        fill_voice_combo(dlg, st, lang, all);
    const bool waiting = all.empty() && k == TtsKind::Azure;
    const bool no_vi = st.ids[1].size() <= 1;
    SetDlgItemTextW(dlg, IDC_TTS_HINT,
        waiting ? L"Đang lấy danh sách giọng từ Azure…"
                : (no_vi && !all.empty() ? L"Máy chưa có giọng tiếng Việt cho bộ máy này. Cài thêm giọng trong Cài đặt Windows > Giọng nói, hoặc dùng Piper hoặc Azure."
                                         : L""));
    if (waiting && st.polls < kMaxVoicePolls)
        SetTimer(dlg, kVoicePollTimer, kVoicePollMs, nullptr);
    else
        KillTimer(dlg, kVoicePollTimer);
}

void read_voice_picks(HWND dlg, DlgState& st, TtsKind k)
{
    for (std::size_t lang = 0; lang < 2; ++lang) {
        const LRESULT i = SendDlgItemMessageW(dlg, kVoiceCombos[lang], CB_GETCURSEL, 0, 0);
        if (i >= 0 && static_cast<std::size_t>(i) < st.ids[lang].size())
            st.prefs.voice[kind_index(k)][lang] = st.ids[lang][static_cast<std::size_t>(i)];
    }
}

void read_rate(HWND dlg, DlgState& st) noexcept
{
    const LRESULT i = SendDlgItemMessageW(dlg, IDC_TTS_RATE, CB_GETCURSEL, 0, 0);
    if (i >= 0 && static_cast<std::size_t>(i) < kTtsSpeeds.size())
        st.prefs.rate = kTtsSpeeds[static_cast<std::size_t>(i)];
}

void init_dialog(HWND dlg, DlgState& st)
{
    st.kinds = st.api->available();
    if (st.kinds.empty())
        st.kinds.push_back(TtsKind::WinRt);
    HWND engines = GetDlgItem(dlg, IDC_TTS_ENGINE);
    for (const TtsKind k : st.kinds)
        add_item(engines, kEngineNames[kind_index(k)]);
    const auto at = std::ranges::find(st.kinds, st.prefs.engine);
    SendMessageW(engines, CB_SETCURSEL, at == st.kinds.end() ? 0 : static_cast<WPARAM>(at - st.kinds.begin()), 0);
    st.prefs.engine = selected_kind(dlg, st);
    for (const wchar_t* label : kTtsSpeedLabels)
        add_item(GetDlgItem(dlg, IDC_TTS_RATE), label);
    SendDlgItemMessageW(dlg, IDC_TTS_RATE, CB_SETCURSEL, static_cast<WPARAM>(nearest_speed_index(st.prefs.rate)), 0);
    fill_voices(dlg, st);
    st.azure.init(dlg, *st.api);
    st.loading = false;
}

void close_dialog(HWND dlg, DlgState& st, int id)
{
    if (id == IDOK && !st.azure.commit(dlg))
        return;
    KillTimer(dlg, kVoicePollTimer);
    st.api->end_preview();
    if (id == IDOK) {
        const TtsKind k = selected_kind(dlg, st);
        read_voice_picks(dlg, st, k);
        read_rate(dlg, st);
        st.prefs.engine = k;
        st.accepted = true;
    }
    EndDialog(dlg, id);
}

void on_engine_changed(HWND dlg, DlgState& st, TtsKind previous)
{
    read_voice_picks(dlg, st, previous);
    st.polls = 0;
    st.api->end_preview();
    fill_voices(dlg, st);
}

void on_preview(HWND dlg, DlgState& st, std::size_t lang)
{
    const LRESULT i = SendDlgItemMessageW(dlg, kVoiceCombos[lang], CB_GETCURSEL, 0, 0);
    if (i < 0 || static_cast<std::size_t>(i) >= st.ids[lang].size())
        return;
    read_rate(dlg, st);
    st.api->preview(selected_kind(dlg, st), st.ids[lang][static_cast<std::size_t>(i)], lang == 1 ? Lang::Vi : Lang::En, st.prefs.rate);
}

INT_PTR on_command(HWND dlg, DlgState& st, WPARAM wp)
{
    const int id = LOWORD(wp), code = HIWORD(wp);
    if (id == IDOK || id == IDCANCEL) {
        if (id == IDCANCEL)
            AzurePage::wipe_key(dlg);
        close_dialog(dlg, st, id);
        return TRUE;
    }
    if (st.loading)
        return FALSE;
    if (id == IDC_TTS_ENGINE && code == CBN_SELCHANGE) {
        on_engine_changed(dlg, st, st.prefs.engine);
        st.prefs.engine = selected_kind(dlg, st);
        return TRUE;
    }
    if (st.azure.on_command(dlg, id, code))
        return TRUE;
    if (code == BN_CLICKED && (id == IDC_TTS_PREVIEW_EN || id == IDC_TTS_PREVIEW_VI)) {
        on_preview(dlg, st, id == IDC_TTS_PREVIEW_VI ? 1 : 0);
        return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) noexcept
{
    try {
        if (msg == WM_INITDIALOG) {
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            init_dialog(dlg, *reinterpret_cast<DlgState*>(lp)); // NOLINT(performance-no-int-to-ptr): Win32 dialog ABI
            return TRUE;
        }
        auto* st = reinterpret_cast<DlgState*>(GetWindowLongPtrW(dlg, DWLP_USER)); // NOLINT(performance-no-int-to-ptr): Win32 dialog ABI
        if (st == nullptr)
            return FALSE;
        if (msg == WM_COMMAND)
            return on_command(dlg, *st, wp);
        if (msg == WM_APP_AZURE_PROBE) {
            st->azure.on_probe(dlg, static_cast<int>(wp));
            return TRUE;
        }
        if (msg == WM_TIMER && wp == kVoicePollTimer) {
            ++st->polls;
            fill_voices(dlg, *st);
            return TRUE;
        }
    } catch (...) {
        log::error("exception in the voice settings dialog");
    }
    return FALSE;
}

}

std::optional<TtsPrefs> show_tts_settings(HINSTANCE inst, HWND owner, const TtsPrefs& initial, TtsDialogApi& api) noexcept
{
    try {
        DlgState st;
        st.prefs = initial;
        st.api = &api;
        DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_TTS_SETTINGS), owner, &dialog_proc, reinterpret_cast<LPARAM>(&st));
        return st.accepted ? std::optional<TtsPrefs>(st.prefs) : std::nullopt;
    } catch (...) {
        log::error("voice settings failed");
        return std::nullopt;
    }
}

}
