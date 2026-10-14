#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>

class Memory;

class HexView {
  public:
    bool create(HWND parent, HFONT font);
    void setFont(HFONT font);
    HWND hwnd() const noexcept { return hwnd_; }

    void attach(const Memory *memory) { memory_ = memory; }
    enum class Follow { Free, PC, Custom };
    void setFollow(Follow mode, uint16_t custom = 0);
    void jumpTo(uint16_t address);
    void refresh();
    uint16_t selectedAddress() const noexcept { return selected_; }
    uint16_t topAddress() const noexcept { return static_cast<uint16_t>(topRow_ * 16); }

    // Memory-access heatmap (item 46). Default on; 'H' or context menu toggles.
    bool heatmapEnabled() const noexcept { return heatmap_; }
    void setHeatmap(bool on) noexcept;

  private:
    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void onPaint(HDC dc, const RECT &rcPaint);
    void paintBuffer(HDC buffer, const RECT &full);
    int rowsVisible(int height) const noexcept;
    void updateScrollbar();
    void syncFollow();
    void drawLegend(HDC buffer, const RECT &rc);

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    int charW_ = 8;
    int lineH_ = 17;
    const Memory *memory_ = nullptr;
    Follow follow_ = Follow::Free;
    uint16_t customBase_ = 0;
    int topRow_ = 0;
    uint16_t selected_ = 0;
    int nibblePhase_ = -1;
    uint64_t lastEpoch_ = 0;
    bool heatmap_ = true;
};
