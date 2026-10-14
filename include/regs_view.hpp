#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

class Emulator;
class Debugger;

class RegsView {
  public:
    bool create(HWND parent, HFONT font, HFONT bold);
    void setFont(HFONT font, HFONT boldFont);
    HWND hwnd() const noexcept { return hwnd_; }
    void attach(Emulator *emulator, Debugger *debugger) {
        emulator_ = emulator;
        debugger_ = debugger;
    }
    void refresh();
    int lineSpacing() const noexcept { return 20; }

  private:
    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void onPaint(HDC dc, const RECT &rcPaint);
    void paintBuffer(HDC buffer, const RECT &full);
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    HFONT bold_ = nullptr;
    Emulator *emulator_ = nullptr;
    Debugger *debugger_ = nullptr;
};
