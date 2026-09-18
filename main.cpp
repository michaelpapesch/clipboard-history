// Clipboard History - a small tray tool that remembers the last copied texts.
//
// Ctrl+V opens a list of the recent clipboard entries instead of pasting right away:
//   Enter / Ctrl+V again   paste the selected entry (initially the latest one)
//   Up/Down, PgUp/PgDn     select another entry
//   mouse click            paste that entry, click its X to remove it
//   Delete                 remove the selected entry
//   Esc / any other key    close
//
// The popup never takes focus, so the application being pasted into keeps its caret. Keys are
// routed to the popup by a low-level keyboard hook while it is visible. If the clipboard holds
// something that is not in the history (image, files, a password-manager secret), Ctrl+V is left
// alone and pastes normally.

#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kAppName[] = L"Clipboard History";
constexpr wchar_t kClassName[] = L"ClipboardHistoryWnd";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"ClipboardHistory";

constexpr size_t kMaxEntries = 30;
constexpr size_t kMaxEntryChars = 256 * 1024;
constexpr int kVisibleRows = 6;

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_SHOW_POPUP = WM_APP + 2; // sent by a second instance
constexpr UINT WM_ACTION = WM_APP + 3;     // posted by the hooks; wParam = Action, lParam = vk

enum Action { ACT_SHOW, ACT_PASTE, ACT_HIDE, ACT_DELETE, ACT_NAV };
enum { IDC_LIST = 100 };
enum { IDM_OPEN = 200, IDM_PAUSE, IDM_AUTOSTART, IDM_CLEAR, IDM_EXIT };
enum { TIMER_SAVE = 1 };

struct Theme {
    COLORREF bg, text, sub, selBg, line, accent;
};

HINSTANCE g_inst;
HWND g_hwnd, g_list;
WNDPROC g_listProc;
HHOOK g_keyboardHook, g_mouseHook;
HWINEVENTHOOK g_foregroundHook;
HICON g_icon;
HFONT g_font, g_smallFont;
HBRUSH g_bgBrush;
Theme g_theme;
UINT g_dpi;
int g_lineHeight = 16;
int g_headerHeight, g_listHeight;
RECT g_clearRect;                    // "Clear all" hit area, set while painting
UINT g_taskbarCreated;
DWORD g_shownTick;
bool g_swallowed[256];               // keys whose key-down the hook has eaten
bool g_pasteOnSelect;                // false when opened from the tray: only copy
bool g_passThrough = true;           // clipboard content is not the top history entry
bool g_paused;
bool g_dirty;

std::vector<std::wstring> g_history; // newest first; list row == index

int S(int v) { return MulDiv(v, static_cast<int>(g_dpi), 96); }

// ---------------------------------------------------------------- storage

std::wstring DataFile() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring dir = std::wstring(buf) + L"\\ClipboardHistory";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\history.dat";
}

DATA_BLOB Entropy() {
    static BYTE entropy[] = "ClipboardHistory.v1";
    return {sizeof(entropy), entropy};
}

// The file is a DPAPI blob (decryptable only by this Windows user). Plain layout:
// "CLPH", u32 version, u32 count, then per entry u32 length + UTF-16 chars.
void SaveHistory() {
    g_dirty = false;
    std::wstring path = DataFile();
    if (path.empty()) return;

    std::vector<BYTE> plain;
    auto append = [&](const void *data, size_t bytes) {
        auto *p = static_cast<const BYTE *>(data);
        plain.insert(plain.end(), p, p + bytes);
    };
    uint32_t header[2] = {1, static_cast<uint32_t>(g_history.size())};
    append("CLPH", 4);
    append(header, sizeof(header));
    for (const auto &entry : g_history) {
        uint32_t len = static_cast<uint32_t>(entry.size());
        append(&len, sizeof(len));
        append(entry.data(), len * sizeof(wchar_t));
    }

    DATA_BLOB in = {static_cast<DWORD>(plain.size()), plain.data()}, entropy = Entropy(), out{};
    BOOL encrypted = CryptProtectData(&in, L"Clipboard history", &entropy, nullptr, nullptr,
                                      CRYPTPROTECT_UI_FORBIDDEN, &out);
    SecureZeroMemory(plain.data(), plain.size());
    if (!encrypted) return;

    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ok = WriteFile(f, out.pbData, out.cbData, &written, nullptr) && written == out.cbData;
        CloseHandle(f);
    }
    LocalFree(out.pbData);

    if (ok) MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.c_str());
}

void ParseHistory(const BYTE *data, size_t size) {
    if (size < 12 || memcmp(data, "CLPH", 4) != 0) return;
    size_t pos = 4;
    auto readU32 = [&](uint32_t &out) {
        if (size - pos < 4) return false;
        memcpy(&out, data + pos, 4);
        pos += 4;
        return true;
    };
    uint32_t version = 0, count = 0;
    if (!readU32(version) || version != 1 || !readU32(count)) return;
    for (uint32_t i = 0; i < count && g_history.size() < kMaxEntries; ++i) {
        uint32_t len = 0;
        if (!readU32(len) || len > kMaxEntryChars || size - pos < len * sizeof(wchar_t)) return;
        std::wstring entry(len, L'\0');
        memcpy(entry.data(), data + pos, len * sizeof(wchar_t));
        pos += len * sizeof(wchar_t);
        if (!entry.empty()) g_history.push_back(std::move(entry));
    }
}

void LoadHistory() {
    std::wstring path = DataFile();
    if (path.empty()) return;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    std::vector<BYTE> blob;
    LARGE_INTEGER size;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 28)) {
        blob.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(f, blob.data(), static_cast<DWORD>(blob.size()), &read, nullptr) || read != blob.size())
            blob.clear();
    }
    CloseHandle(f);
    if (blob.empty()) return;

    DATA_BLOB in = {static_cast<DWORD>(blob.size()), blob.data()}, entropy = Entropy(), out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return;
    ParseHistory(out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
}

void ScheduleSave() {
    g_dirty = true;
    SetTimer(g_hwnd, TIMER_SAVE, 2000, nullptr);
}

// -------------------------------------------------------------- clipboard

void RefreshPopup();
void HidePopup();

// Single-line-friendly version of an entry: whitespace runs collapsed, length capped.
std::wstring Preview(const std::wstring &s) {
    std::wstring out;
    bool space = false;
    for (wchar_t c : s) {
        if (out.size() >= 300) break;
        if (c < 32 || iswspace(c)) {
            space = !out.empty();
        } else {
            if (space) out += L' ';
            space = false;
            out += c;
        }
    }
    return out;
}

bool OpenClipboardRetry() {
    for (int i = 0; i < 8; ++i) {
        if (OpenClipboard(g_hwnd)) return true;
        Sleep(10);
    }
    return false;
}

// Returns false if the text is not something we keep (empty, blank, huge).
bool AddEntry(std::wstring text) {
    if (text.size() > kMaxEntryChars || Preview(text).empty()) return false;
    auto it = std::find(g_history.begin(), g_history.end(), text);
    if (it == g_history.begin() && it != g_history.end()) return true;
    if (it != g_history.end()) g_history.erase(it);
    g_history.insert(g_history.begin(), std::move(text));
    if (g_history.size() > kMaxEntries) g_history.resize(kMaxEntries);
    ScheduleSave();
    if (IsWindowVisible(g_hwnd)) RefreshPopup();
    return true;
}

// Returns true if the clipboard text is now the top history entry.
bool CaptureClipboard() {
    // Password managers mark secrets with these formats; respect them.
    static const UINT fmtExclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static const UINT fmtCanInclude = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");

    if (IsClipboardFormatAvailable(fmtExclude) || !IsClipboardFormatAvailable(CF_UNICODETEXT)) return false;
    if (!OpenClipboardRetry()) return false;

    bool allowed = true;
    if (HANDLE h = GetClipboardData(fmtCanInclude)) {
        if (GlobalSize(h) >= sizeof(DWORD)) {
            if (auto *flag = static_cast<const DWORD *>(GlobalLock(h))) {
                allowed = *flag != 0;
                GlobalUnlock(h);
            }
        }
    }

    std::wstring text;
    if (allowed) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (auto *p = static_cast<const wchar_t *>(GlobalLock(h))) {
                size_t maxChars = GlobalSize(h) / sizeof(wchar_t);
                text.assign(p, wcsnlen(p, maxChars));
                GlobalUnlock(h);
            }
        }
    }
    CloseClipboard();
    return AddEntry(std::move(text));
}

// Empty text clears the clipboard.
bool SetClipboardText(const std::wstring &text) {
    if (!OpenClipboardRetry()) return false;
    bool ok = EmptyClipboard() != FALSE;
    if (!text.empty()) {
        ok = false;
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void *p = GlobalLock(mem)) {
                memcpy(p, text.c_str(), bytes);
                GlobalUnlock(mem);
                ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            }
            if (!ok) GlobalFree(mem);
        }
    }
    CloseClipboard();
    return ok;
}

void SendCtrlV() {
    // The user may still be holding Ctrl from the Ctrl+V that opened the popup; releasing it
    // behind their back would turn their next V into a plain letter.
    bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    INPUT in[4]{};
    int n = 0;
    auto key = [&](WORD vk, DWORD flags) {
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = vk;
        in[n].ki.dwFlags = flags;
        ++n;
    };
    if (!ctrlHeld) key(VK_CONTROL, 0);
    key('V', 0);
    key('V', KEYEVENTF_KEYUP);
    if (!ctrlHeld) key(VK_CONTROL, KEYEVENTF_KEYUP);
    SendInput(n, in, sizeof(INPUT)); // injected, so our own hook lets it through
}

// ------------------------------------------------------------------ popup

bool IsDarkMode() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

void ApplyTheme() {
    bool dark = IsDarkMode();
    g_theme = dark ? Theme{RGB(32, 32, 32), RGB(240, 240, 240), RGB(160, 160, 160), RGB(56, 56, 56),
                           RGB(64, 64, 64), RGB(76, 194, 255)}
                   : Theme{RGB(249, 249, 249), RGB(26, 26, 26), RGB(96, 96, 96), RGB(230, 230, 230),
                           RGB(222, 222, 222), RGB(0, 103, 192)};
    if (g_bgBrush) DeleteObject(g_bgBrush);
    g_bgBrush = CreateSolidBrush(g_theme.bg);
    SetWindowTheme(g_list, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

HFONT MakeFont(int px) {
    return CreateFontW(-S(px), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

int ItemHeight() { return 2 * g_lineHeight + S(16); }

void ApplyDpi(UINT dpi) {
    if (dpi == g_dpi && g_font) return;
    g_dpi = dpi;
    if (g_font) DeleteObject(g_font);
    if (g_smallFont) DeleteObject(g_smallFont);
    g_font = MakeFont(14);
    g_smallFont = MakeFont(12);

    HDC dc = GetDC(g_hwnd);
    HGDIOBJ old = SelectObject(dc, g_font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    g_lineHeight = tm.tmHeight;
    SelectObject(dc, old);
    ReleaseDC(g_hwnd, dc);

    SendMessageW(g_list, LB_SETITEMHEIGHT, 0, ItemHeight());
}

// Refills the list and sizes the window to its rows; returns the window size.
SIZE LayoutPopup() {
    int count = static_cast<int>(g_history.size());
    int sel = static_cast<int>(SendMessageW(g_list, LB_GETCURSEL, 0, 0));

    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < count; ++i) SendMessageW(g_list, LB_ADDSTRING, 0, i);
    SendMessageW(g_list, LB_SETCURSEL, std::clamp(sel, 0, std::max(0, count - 1)), 0);
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);

    g_headerHeight = S(36);
    g_listHeight = std::clamp(count, 1, kVisibleRows) * ItemHeight();
    SIZE size = {S(420), g_headerHeight + g_listHeight + S(30)};
    MoveWindow(g_list, 1, g_headerHeight, size.cx - 2, g_listHeight, FALSE);
    SetWindowPos(g_hwnd, nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_hwnd, nullptr, TRUE);
    return size;
}

// The history changed while the popup is open.
void RefreshPopup() {
    if (g_history.empty()) HidePopup();
    else LayoutPopup();
}

LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN)) {
        RECT rc;
        GetWindowRect(g_hwnd, &rc);
        if (!PtInRect(&rc, reinterpret_cast<const MSLLHOOKSTRUCT *>(lp)->pt)) PostMessageW(g_hwnd, WM_ACTION, ACT_HIDE, 0);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void HidePopup() {
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    if (IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, SW_HIDE);
}

// forPaste: opened by Ctrl+V, selecting an entry pastes it. Otherwise (tray) it is only copied.
void ShowPopup(bool forPaste) {
    if (g_history.empty()) return;
    g_pasteOnSelect = forPaste;

    // Anchor at the text caret of the focused window when it exposes one.
    RECT caret{};
    bool haveCaret = false;
    HMONITOR monitor;
    HWND target = forPaste ? GetForegroundWindow() : nullptr;
    if (target) {
        GUITHREADINFO gti{};
        gti.cbSize = sizeof(gti);
        if (GetGUIThreadInfo(GetWindowThreadProcessId(target, nullptr), &gti) && gti.hwndCaret &&
            !IsRectEmpty(&gti.rcCaret)) {
            caret = gti.rcCaret;
            MapWindowPoints(gti.hwndCaret, nullptr, reinterpret_cast<POINT *>(&caret), 2);
            haveCaret = true;
        }
        monitor = haveCaret ? MonitorFromRect(&caret, MONITOR_DEFAULTTONEAREST)
                            : MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    } else {
        POINT pt;
        GetCursorPos(&pt);
        monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor, &mi);
    const RECT &work = mi.rcWork;

    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    ApplyDpi(dpiX);
    ApplyTheme();

    SendMessageW(g_list, LB_SETCURSEL, 0, 0);
    SIZE size = LayoutPopup();
    SendMessageW(g_list, LB_SETTOPINDEX, 0, 0);

    int w = size.cx, h = size.cy, x, y;
    if (haveCaret) {
        x = caret.left;
        y = caret.bottom + S(6);
        if (y + h > work.bottom) y = caret.top - h - S(6);
    } else if (target) {
        x = (work.left + work.right - w) / 2;
        y = (work.top + work.bottom - h) / 2;
    } else {
        x = work.right - w - S(12);
        y = work.bottom - h - S(12);
    }
    x = std::clamp(x, static_cast<int>(work.left), std::max<int>(work.left, work.right - w));
    y = std::clamp(y, static_cast<int>(work.top), std::max<int>(work.top, work.bottom - h));

    g_shownTick = GetTickCount();
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (!g_mouseHook) g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, g_inst, 0);
}

int SelectedEntry() {
    LRESULT sel = SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    return sel >= 0 && static_cast<size_t>(sel) < g_history.size() ? static_cast<int>(sel) : -1;
}

void MoveSelection(int delta) {
    int count = static_cast<int>(SendMessageW(g_list, LB_GETCOUNT, 0, 0));
    if (count <= 0) return;
    int sel = static_cast<int>(SendMessageW(g_list, LB_GETCURSEL, 0, 0));
    SendMessageW(g_list, LB_SETCURSEL, std::clamp(sel + delta, 0, count - 1), 0);
}

void Navigate(DWORD vk) {
    switch (vk) {
    case VK_DOWN: MoveSelection(1); break;
    case VK_UP: MoveSelection(-1); break;
    case VK_NEXT: MoveSelection(kVisibleRows - 1); break;
    case VK_PRIOR: MoveSelection(-(kVisibleRows - 1)); break;
    case VK_HOME: MoveSelection(-static_cast<int>(kMaxEntries)); break;
    case VK_END: MoveSelection(static_cast<int>(kMaxEntries)); break;
    }
}

void PasteSelected() {
    int idx = SelectedEntry();
    if (idx < 0) return;
    std::wstring text = g_history[idx]; // copy: the clipboard update reorders g_history
    bool paste = g_pasteOnSelect;
    HidePopup();
    if (SetClipboardText(text) && paste) SendCtrlV();
}

void DeleteEntry(int idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= g_history.size()) return;
    g_history.erase(g_history.begin() + idx);
    // The top entry is what the clipboard holds; keep the two in sync so a removed text is really gone.
    if (idx == 0 && !g_passThrough) SetClipboardText(g_history.empty() ? L"" : g_history.front());
    ScheduleSave();
    RefreshPopup();
}

void ClearHistory() {
    g_history.clear();
    if (!g_passThrough) SetClipboardText(L"");
    SaveHistory();
    HidePopup();
}

LRESULT CALLBACK KeyboardHook(int code, WPARAM wp, LPARAM lp) {
    auto *k = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lp);
    if (code != HC_ACTION || (k->flags & LLKHF_INJECTED) || k->vkCode >= 256)
        return CallNextHookEx(nullptr, code, wp, lp);

    DWORD vk = k->vkCode;
    if (k->flags & LLKHF_UP) {
        if (!g_swallowed[vk]) return CallNextHookEx(nullptr, code, wp, lp);
        g_swallowed[vk] = false;
        return 1;
    }

    bool isNav = vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END;
    if (g_swallowed[vk]) { // auto-repeat of a key we already handle
        if (isNav) PostMessageW(g_hwnd, WM_ACTION, ACT_NAV, vk);
        return 1;
    }

    auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    bool visible = IsWindowVisible(g_hwnd) != FALSE;
    Action action;
    if (vk == 'V' && down(VK_CONTROL) && !down(VK_SHIFT) && !down(VK_MENU) && !down(VK_LWIN) && !down(VK_RWIN)) {
        if (!visible && (g_paused || g_passThrough || g_history.empty()))
            return CallNextHookEx(nullptr, code, wp, lp); // normal paste
        action = visible ? ACT_PASTE : ACT_SHOW;
    } else if (!visible) {
        return CallNextHookEx(nullptr, code, wp, lp);
    } else if (isNav) {
        action = ACT_NAV;
    } else if (vk == VK_RETURN) {
        action = ACT_PASTE;
    } else if (vk == VK_ESCAPE) {
        action = ACT_HIDE;
    } else if (vk == VK_DELETE) {
        action = ACT_DELETE;
    } else {
        bool modifier = vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= VK_LSHIFT && vk <= VK_RMENU) ||
                        vk == VK_LWIN || vk == VK_RWIN;
        if (!modifier) PostMessageW(g_hwnd, WM_ACTION, ACT_HIDE, 0); // user went on typing
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    g_swallowed[vk] = true;
    PostMessageW(g_hwnd, WM_ACTION, action, vk);
    return 1;
}

void CALLBACK ForegroundChanged(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (hwnd != g_hwnd && IsWindowVisible(g_hwnd) && GetTickCount() - g_shownTick > 250) HidePopup();
}

int DeleteZoneWidth() { return S(40); }

int RowFromPoint(LPARAM lp) {
    POINT pt = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
    LRESULT hit = SendMessageW(g_list, LB_ITEMFROMPOINT, 0, lp);
    RECT rc;
    if (HIWORD(hit) || SendMessageW(g_list, LB_GETITEMRECT, LOWORD(hit), reinterpret_cast<LPARAM>(&rc)) == LB_ERR ||
        !PtInRect(&rc, pt))
        return -1;
    return LOWORD(hit);
}

// The stock list box would take focus on click; handle the mouse ourselves, menu style.
LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEMOVE: {
        int row = RowFromPoint(lp);
        if (row >= 0 && row != SendMessageW(hwnd, LB_GETCURSEL, 0, 0)) SendMessageW(hwnd, LB_SETCURSEL, row, 0);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int row = RowFromPoint(lp);
        if (row < 0) return 0;
        SendMessageW(hwnd, LB_SETCURSEL, row, 0);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (static_cast<short>(LOWORD(lp)) >= rc.right - DeleteZoneWidth()) DeleteEntry(row);
        else PasteSelected();
        return 0;
    }
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        return 0;
    }
    return CallWindowProcW(g_listProc, hwnd, msg, wp, lp);
}

void DrawItem(const DRAWITEMSTRUCT *d) {
    if (d->itemID == static_cast<UINT>(-1) || d->itemID >= g_history.size()) return;
    bool selected = (d->itemState & ODS_SELECTED) != 0;
    RECT rc = d->rcItem;
    HBRUSH dcBrush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));

    SetDCBrushColor(d->hDC, selected ? g_theme.selBg : g_theme.bg);
    FillRect(d->hDC, &rc, dcBrush);
    if (selected) {
        RECT bar = {rc.left + S(4), rc.top + S(10), rc.left + S(7), rc.bottom - S(10)};
        SetDCBrushColor(d->hDC, g_theme.accent);
        FillRect(d->hDC, &bar, dcBrush);

        // Remove "X"
        int cx = rc.right - DeleteZoneWidth() / 2, cy = (rc.top + rc.bottom) / 2, r = S(4);
        HPEN pen = CreatePen(PS_SOLID, std::max(1, S(1)), g_theme.sub);
        HGDIOBJ oldPen = SelectObject(d->hDC, pen);
        MoveToEx(d->hDC, cx - r, cy - r, nullptr);
        LineTo(d->hDC, cx + r + 1, cy + r + 1);
        MoveToEx(d->hDC, cx + r, cy - r, nullptr);
        LineTo(d->hDC, cx - r - 1, cy + r + 1);
        SelectObject(d->hDC, oldPen);
        DeleteObject(pen);
    } else {
        RECT line = {rc.left + S(16), rc.bottom - 1, rc.right - S(16), rc.bottom};
        SetDCBrushColor(d->hDC, g_theme.line);
        FillRect(d->hDC, &line, dcBrush);
    }

    std::wstring text = Preview(g_history[d->itemID]);
    RECT textRc = {rc.left + S(16), rc.top + S(8), rc.right - DeleteZoneWidth(), rc.bottom - S(8)};
    HGDIOBJ oldFont = SelectObject(d->hDC, g_font);
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, g_theme.text);
    DrawTextW(d->hDC, text.c_str(), static_cast<int>(text.size()), &textRc,
              DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(d->hDC, oldFont);
}

void PaintPopup(HDC dc) {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    HBRUSH dcBrush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    FillRect(dc, &rc, g_bgBrush);
    SetDCBrushColor(dc, g_theme.line);
    FrameRect(dc, &rc, dcBrush);
    RECT sep = {rc.left, g_headerHeight - 1, rc.right, g_headerHeight};
    FillRect(dc, &sep, dcBrush);
    sep.top = g_headerHeight + g_listHeight;
    sep.bottom = sep.top + 1;
    FillRect(dc, &sep, dcBrush);

    HGDIOBJ oldFont = SelectObject(dc, g_smallFont);
    SetBkMode(dc, TRANSPARENT);

    RECT header = {S(16), 0, rc.right - S(16), g_headerHeight};
    SetTextColor(dc, g_theme.sub);
    DrawTextW(dc, L"Clipboard history", -1, &header, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

    const wchar_t clearText[] = L"Clear all";
    SIZE clearSize;
    GetTextExtentPoint32W(dc, clearText, lstrlenW(clearText), &clearSize);
    g_clearRect = {header.right - clearSize.cx - S(8), S(4), header.right + S(8), g_headerHeight - S(4)};
    SetTextColor(dc, g_theme.accent);
    DrawTextW(dc, clearText, -1, &header, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);

    RECT footer = {S(16), g_headerHeight + g_listHeight, rc.right - S(16), rc.bottom};
    SetTextColor(dc, g_theme.sub);
    DrawTextW(dc, g_pasteOnSelect ? L"Enter  paste     Del  remove     Esc  close"
                                  : L"Enter  copy     Del  remove     Esc  close",
              -1, &footer, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    SelectObject(dc, oldFont);
}

// ------------------------------------------------------------------- tray

// Draws a small clipboard glyph so the exe needs no .ico resource.
HICON CreateAppIcon(int size) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) return LoadIconW(nullptr, IDI_APPLICATION);
    auto *px = static_cast<uint32_t *>(bits);
    std::fill(px, px + size * size, 0u);

    // Coordinates are on a 16x16 grid.
    auto fill = [&](double x0, double y0, double x1, double y1, uint32_t argb) {
        int l = static_cast<int>(x0 * size / 16 + 0.5), t = static_cast<int>(y0 * size / 16 + 0.5);
        int r = static_cast<int>(x1 * size / 16 + 0.5), b = static_cast<int>(y1 * size / 16 + 0.5);
        for (int y = std::max(t, 0); y < std::min(b, size); ++y)
            for (int x = std::max(l, 0); x < std::min(r, size); ++x) px[y * size + x] = argb;
    };
    fill(2, 2, 14, 16, 0xFF2B7CD3);         // board
    fill(3.5, 4.5, 12.5, 14.5, 0xFFFFFFFF); // paper
    fill(5, 0.5, 11, 4, 0xFF1B4F8A);        // clip
    fill(5, 6.5, 11, 7.5, 0xFF8FAFD0);
    fill(5, 9, 11, 10, 0xFF8FAFD0);
    fill(5, 11.5, 9, 12.5, 0xFF8FAFD0);

    std::vector<uint8_t> maskBits(((size + 15) / 16) * 2 * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

void AddTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_icon;
    lstrcpynW(nid.szTip, L"Clipboard History (Ctrl+V)", ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

bool IsAutostartEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) ==
           ERROR_SUCCESS;
}

void SetAutostart(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enable) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE *>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

void ShowTrayMenu() {
    HidePopup();
    UINT grayIfEmpty = g_history.empty() ? MF_GRAYED : 0;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | grayIfEmpty, IDM_OPEN, L"Open history");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_paused ? MF_CHECKED : 0), IDM_PAUSE, L"Pause (Ctrl+V pastes normally)");
    AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0), IDM_AUTOSTART, L"Start with Windows");
    AppendMenuW(menu, MF_STRING | grayIfEmpty, IDM_CLEAR, L"Clear history");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");
    SetMenuDefaultItem(menu, IDM_OPEN, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd); // otherwise the menu doesn't close when clicking elsewhere
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// ----------------------------------------------------------------- window

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        g_list = CreateWindowExW(0, L"LISTBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT, 0, 0,
                                 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)), g_inst, nullptr);
        g_listProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_list, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ListProc)));

        int cornerRound = 2; // DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND (Windows 11)
        DwmSetWindowAttribute(hwnd, 33, &cornerRound, sizeof(cornerRound));

        ApplyTheme();
        ApplyDpi(96);
        AddClipboardFormatListener(hwnd);
        g_passThrough = !CaptureClipboard();
        g_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                           ForegroundChanged, 0, 0, WINEVENT_OUTOFCONTEXT);
        g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, g_inst, 0);
        if (!g_keyboardHook)
            MessageBoxW(nullptr,
                        L"Could not install the keyboard hook, so Ctrl+V will paste normally.\n"
                        L"The history is still available from the tray icon.",
                        kAppName, MB_ICONWARNING | MB_TOPMOST);
        AddTrayIcon();
        return 0;
    }

    case WM_CLIPBOARDUPDATE:
        g_passThrough = g_paused || !CaptureClipboard();
        return 0;

    case WM_ACTION:
        switch (wp) {
        case ACT_SHOW: ShowPopup(true); break;
        case ACT_PASTE: PasteSelected(); break;
        case ACT_HIDE: HidePopup(); break;
        case ACT_DELETE: DeleteEntry(SelectedEntry()); break;
        case ACT_NAV: Navigate(static_cast<DWORD>(lp)); break;
        }
        return 0;

    case WM_SHOW_POPUP:
        ShowPopup(false);
        return 0;

    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) ShowPopup(false);
        else if (LOWORD(lp) == WM_RBUTTONUP) ShowTrayMenu();
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_LBUTTONDOWN: {
        POINT pt = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        if (PtInRect(&g_clearRect, pt)) ClearHistory();
        return 0;
    }

    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (reinterpret_cast<HWND>(wp) == hwnd && PtInRect(&g_clearRect, pt)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN: ShowPopup(false); break;
        case IDM_PAUSE:
            g_paused = !g_paused;
            g_passThrough = g_paused || !CaptureClipboard();
            break;
        case IDM_AUTOSTART: SetAutostart(!IsAutostartEnabled()); break;
        case IDM_CLEAR:
            if (MessageBoxW(nullptr, L"Delete all clipboard history entries?", kAppName,
                            MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDYES)
                ClearHistory();
            break;
        case IDM_EXIT: DestroyWindow(hwnd); break;
        }
        return 0;

    case WM_TIMER:
        KillTimer(hwnd, wp);
        if (wp == TIMER_SAVE) SaveHistory();
        return 0;

    case WM_MEASUREITEM:
        reinterpret_cast<MEASUREITEMSTRUCT *>(lp)->itemHeight = 2 * g_lineHeight + 16;
        return TRUE;

    case WM_DRAWITEM:
        DrawItem(reinterpret_cast<const DRAWITEMSTRUCT *>(lp));
        return TRUE;

    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, g_theme.text);
        SetBkColor(dc, g_theme.bg);
        return reinterpret_cast<LRESULT>(g_bgBrush);
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        PaintPopup(BeginPaint(hwnd, &ps));
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CLOSE:
        HidePopup();
        return 0;

    case WM_ENDSESSION:
        if (wp && g_dirty) SaveHistory();
        return 0;

    case WM_DESTROY:
        HidePopup();
        if (g_keyboardHook) UnhookWindowsHookEx(g_keyboardHook);
        if (g_foregroundHook) UnhookWinEvent(g_foregroundHook);
        if (g_dirty) SaveHistory();
        RemoveClipboardFormatListener(hwnd);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    if (msg == g_taskbarCreated && g_taskbarCreated) {
        AddTrayIcon(); // Explorer restarted
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\ClipboardHistory.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND running = FindWindowW(kClassName, nullptr)) PostMessageW(running, WM_SHOW_POPUP, 0, 0);
        return 0;
    }

    g_inst = inst;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_icon = CreateAppIcon(GetSystemMetrics(SM_CXSMICON));
    LoadHistory();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = g_icon;
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return 1;

    if (!CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClassName, kAppName,
                         WS_POPUP | WS_CLIPCHILDREN, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr))
        return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
