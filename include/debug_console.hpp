#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>
#include <deque>
#include <string>

class DebugConsole {
  public:
    bool create(HWND parent, HFONT font);
    void setFont(HFONT font);
    HWND hwnd() const noexcept { return hwnd_; }

    void append(const std::string &text);
    void clear();

    void setVisible(bool v);
    bool isVisible() const noexcept { return visible_; }
    void toggle() { setVisible(!visible_); }

  private:
    struct Line {
        std::string text;
    };

    void onPaint(HDC dc, const RECT &rcPaint);
    void paintBuffer(HDC buffer, const RECT &full);
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    int charW_ = 8;
    int lineH_ = 16;
    std::deque<std::string> lines_;
    int scrollOffset_ = 0;
    bool stickToBottom_ = true;
    bool visible_ = false;

    // Selection state for copying debug/serial output.  Values are line indices.
    int selectAnchor_ = -1;
    int selectEnd_ = -1;
    bool selecting_ = false;
};