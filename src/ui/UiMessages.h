#pragma once
#include <windows.h>

namespace eb {

inline constexpr UINT kMsgRescan = WM_APP + 1;
inline constexpr UINT WM_APP_SCAN_PROGRESS = WM_APP + 2; // wParam = files done, lParam = total
inline constexpr UINT WM_APP_SCAN_DONE = WM_APP + 3;     // wParam bit 0 = finished without error, bit 1 = the library changed
inline constexpr UINT WM_APP_OPEN_BOOK = WM_APP + 4;     // the id is taken from MainWnd::take_open_request
inline constexpr UINT WM_APP_COVER_READY = WM_APP + 5; // covers are waiting in the CoverWorker queue

inline constexpr UINT WM_APP_PAGE_READY = WM_APP + 20;   // RenderWorker -> ReaderView: results are waiting
inline constexpr UINT WM_APP_READER_EVENT = WM_APP + 21; // ReaderView -> MainWnd; wParam = ReaderEvent, lParam = page
inline constexpr UINT kMsgSmokeOpen = WM_APP + 22;

inline constexpr UINT WM_APP_TRAY = WM_APP + 30;     // Shell -> MainWnd; NOTIFYICON_VERSION_4 callback
inline constexpr UINT WM_APP_TTS_CMD = WM_APP + 31;  // tray -> MainWnd; wParam = TtsCmd
inline constexpr UINT WM_APP_TO_TRAY = WM_APP + 32;  // MainWnd -> ReaderView: flush progress and release GPU/cache memory
inline constexpr UINT WM_APP_OPEN_PATH = WM_APP + 33; // a file to open is pending in MainWnd
inline constexpr UINT WM_APP_EXTERNAL_META = WM_APP + 34; // ExternalOpen -> MainWnd: metadata of an outside book is ready
inline constexpr UINT WM_APP_TTS_EVENT = WM_APP + 35;      // TtsEngine -> MainWnd: wParam = utterance id, lParam = pack_tts

// Text: lParam is the page whose text was requested (not the shown page).
// PageShown: the visible page number changed (optimistic); PageSettled: the worker confirmed it and progress was recorded.
// Relayout: font, theme, size or zoom changed, so page numbers and texts are stale; Relaid: the first page of the new layout is shown.
enum class ReaderEvent : unsigned char { PageShown, PageSettled, OpenFailed, CloseRequested, Fullscreen, Settings, Toc, Bookmark, GoTo, Text, Relayout, Relaid };

inline constexpr int kIdBack = 108;
inline constexpr int kIdResumeText = 109;
inline constexpr int kIdResumeStart = 110;
inline constexpr int kIdReaderSettings = 111;
inline constexpr int kIdTocPane = 112;
inline constexpr int kIdChapterCombo = 121;
inline constexpr int kIdTtsPlay = 113;
inline constexpr int kIdTtsStop = 114;
inline constexpr int kIdTtsPrev = 115;
inline constexpr int kIdTtsNext = 116;
inline constexpr int kIdTtsSpeed = 117;
inline constexpr int kIdTtsSettings = 118;
inline constexpr int kIdTtsInfo = 119;

inline constexpr UINT_PTR kSearchTimer = 1;
inline constexpr UINT_PTR kReaderTickTimer = 20;
inline constexpr UINT_PTR kResumeBarTimer = 21;
inline constexpr UINT_PTR kSmokeTimer = 22;
inline constexpr UINT_PTR kSoakTimer = 23;
inline constexpr UINT kSearchDebounceMs = 150;

}
