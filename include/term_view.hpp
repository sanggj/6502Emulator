#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>
#include <deque>
#include <string>

class TermView {
  public:
    enum class Role { Info, Echo, Ok, Warn, Err, Dim };

    bool create(HWND parent, HFONT font);
    void setFont(HFONT font);
    HWND hwnd() const noexcept { return hwnd_; }

    void append(Role role, const std::string &text);
    void clear();
    std::string lastLineText() const;

  private:
    struct Line {
        Role role;
        std::string text;
    };

    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void onPaint(HDC dc, const RECT &rcPaint);
    void paintBuffer(HDC buffer, const RECT &full);
    int rowsVisible(int height) const noexcept;
    static COLORREF roleColor(Role role);

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    int charW_ = 8;
    int lineH_ = 17;
    std::deque<Line> lines_;
    bool stickToBottom_ = true;
};
