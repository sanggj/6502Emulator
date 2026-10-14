#include "win_util.hpp"

#include <commdlg.h>
#include <cstring>

namespace winutil {

int textWidth(HDC dc, const std::string &text) {
    SIZE sz{};
    GetTextExtentPoint32A(dc, text.c_str(), static_cast<int>(text.size()), &sz);
    return sz.cx;
}

std::string clipboardGet(HWND owner) {
    if (!OpenClipboard(owner)) return {};
    HANDLE handle = GetClipboardData(CF_TEXT);
    std::string out;
    if (handle) {
        const char *data = static_cast<const char *>(GlobalLock(handle));
        if (data) {
            out.assign(data);
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return out;
}

void clipboardSet(HWND owner, const std::string &text) {
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    const HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (global) {
        char *target = static_cast<char *>(GlobalLock(global));
        if (target) {
            std::memcpy(target, text.c_str(), text.size() + 1);
            GlobalUnlock(global);
            SetClipboardData(CF_TEXT, global);
        }
    }
    CloseClipboard();
}

bool openFileDialog(HWND owner, std::string &out, const char *filter) {
    char path[MAX_PATH]{};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameA(&ofn)) return false;
    out = path;
    return true;
}

bool saveFileDialog(HWND owner, std::string &out, const char *filter, const char *defaultExt) {
    char path[MAX_PATH]{};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defaultExt;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameA(&ofn)) return false;
    out = path;
    return true;
}

void drawTextOut(HDC dc, int x, int y, const std::string &text, COLORREF fg) {
    SetTextColor(dc, fg);
    SetBkMode(dc, TRANSPARENT);
    TextOutA(dc, x, y, text.c_str(), static_cast<int>(text.size()));
}

void fillRectColor(HDC dc, const RECT &rc, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
}

void frameRectColor(HDC dc, const RECT &rc, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, &rc, brush);
    DeleteObject(brush);
}

} // namespace winutil
