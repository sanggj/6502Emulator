#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <string>

namespace winutil {

int textWidth(HDC dc, const std::string &text);
std::string clipboardGet(HWND owner);
void clipboardSet(HWND owner, const std::string &text);
bool openFileDialog(HWND owner, std::string &out, const char *filter);
bool saveFileDialog(HWND owner, std::string &out, const char *filter, const char *defaultExt);
void drawTextOut(HDC dc, int x, int y, const std::string &text, COLORREF fg);
void fillRectColor(HDC dc, const RECT &rc, COLORREF color);
void frameRectColor(HDC dc, const RECT &rc, COLORREF color);

} // namespace winutil
