#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class Emulator;
class Debugger;

class DisasmView {
  public:
    bool create(HWND parent, HFONT font);
    void setFont(HFONT font);
    HWND hwnd() const noexcept { return hwnd_; }

    void attach(Emulator *emulator, Debugger *debugger);
    void refresh(bool force = false);
    void centerOn(uint16_t address);
    void jumpToPc();
    uint16_t selectedAddress() const noexcept { return selected_; }

  private:
    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void onPaint(HDC dc, const RECT &rcPaint);
    void paintBuffer(HDC buffer, const RECT &full);
    int rowsVisible(int height) const noexcept;
    void rebuildCacheFrom(uint16_t start);
    void updateScrollbar();

    // Hover tooltip (item 10): per-instruction cycle breakdown.
    std::string tipTextFor(uint16_t addr);
    void showTip(uint16_t addr, int clientX, int clientY);
    void hideTip();
    static LRESULT CALLBACK tipProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    int charW_ = 8;
    int lineH_ = 17;
    Emulator *emulator_ = nullptr;
    Debugger *debugger_ = nullptr;
    std::vector<uint16_t> rowAddrs_;
    int topRow_ = 0;
    uint16_t selected_ = 0x8000;
    uint16_t lastPc_ = 0xFFFF;

    HWND tip_ = nullptr;
    std::string tipText_;
    bool tipTracked_ = false; // TrackMouseEvent armed
    std::unordered_map<uint16_t, std::string> tipCache_;
};
