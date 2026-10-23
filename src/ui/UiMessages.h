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

enum class ReaderEvent : WPARAM { PageShown, OpenFailed, CloseRequested, Fullscreen };

inline constexpr UINT_PTR kSearchTimer = 1;
inline constexpr UINT_PTR kReaderTickTimer = 20;
inline constexpr UINT_PTR kResumeBarTimer = 21;
inline constexpr UINT_PTR kSmokeTimer = 22;
inline constexpr UINT kSearchDebounceMs = 150;

}
