#pragma once
#include "TtsSettings.h"
#include "tts/azure/AzureProbe.h"

#include <windows.h>

#include <string>

namespace eb {

// The Azure part of the voice dialog: key, region, tier, quota, usage counter and the connection test.
class AzurePage {
public:
    void init(HWND dlg, TtsDialogApi& api);
    // True when the command belonged to this page.
    bool on_command(HWND dlg, int id, int code);
    void on_probe(HWND dlg, int status);
    // Validates and stores the fields. False (with a message shown) keeps the dialog open.
    bool commit(HWND dlg);

private:
    void show_view(HWND dlg);
    void show_key_state(HWND dlg);
    void delete_key(HWND dlg);
    void reset_counter(HWND dlg);
    void test_connection(HWND dlg);
    bool store_key(HWND dlg);
    bool read_edits(HWND dlg, AzureEdits& out);

    TtsDialogApi* api_ = nullptr;
    AzureProbe probe_;
    std::wstring used_text_;
    bool key_changed_ = false;
};

}
