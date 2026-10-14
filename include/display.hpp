#pragma once

#ifdef _WIN32
#include <windows.h>
#endif
#include <cstdint>
#include <string>
#include <vector>

class Display {
  public:
    Display();
    ~Display();

    bool createChild(
#ifdef _WIN32
        HWND parent,
#else
        void *parent,
#endif
        int width, int height);
    void destroy();
    void resize(int width, int height);

    void renderMemory(const uint8_t *memory, uint16_t baseAddress, int sourceWidth, int sourceHeight);
    void clear(uint32_t argb = 0xFF000000u);

    bool running() const noexcept { return running_; }
#ifdef _WIN32
    HWND hwnd() const noexcept { return hwnd_; }
#endif
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }

    bool keyDown(int virtualKey) const noexcept;
    bool isFocused() const noexcept { return focused_; }
    void setFocused(bool f) noexcept { focused_ = f; }
    const std::vector<uint32_t> &frame() const noexcept { return framebuffer_; }

    void setPaletteEntry(int index, uint32_t argb);
    uint32_t paletteEntry(int index) const noexcept;
    void resetPalette();

    void setSmoothScaling(bool smooth) noexcept { smooth_ = smooth; }
    bool smoothScaling() const noexcept { return smooth_; }

    int mouseX() const noexcept { return mouseX_; }
    int mouseY() const noexcept { return mouseY_; }
    bool mouseLeft() const noexcept { return mouseLeft_; }
    bool mouseRight() const noexcept { return mouseRight_; }

    bool saveScreenshot(const std::string &path, std::string &error) const;

  private:
#ifdef _WIN32
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    void ensureDib(HDC dc);
    void blit();
    // Focus mode confines the pointer to this view so stray clicks cannot
    // reach window chrome (e.g. the close button) while the cursor is hidden.
    void confineMouse();
    void releaseMouse();
    void enterFocusMode();
    void exitFocusMode(bool refocusParent);
#endif

#ifdef _WIN32
    HWND hwnd_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HDC memoryDc_ = nullptr;
    void *bitmapBits_ = nullptr;
    int dibWidth_ = 0;
    int dibHeight_ = 0;

    // Dirty-rect tracking (item 55): per-64px source block.
    static constexpr int kBlockPx = 64;
    std::vector<uint8_t> srcCache_;   // last bytes rendered into framebuffer_
    std::vector<uint8_t> blockDirty_; // one flag per source block
    int blockCols_ = 0;
    int blockRows_ = 0;
    bool srcCacheValid_ = false;
    int paletteVersion_ = 0;
    int cachePaletteVersion_ = 0;
    int prevDibW_ = 0; // full-copy trigger on DIB resize
    int prevDibH_ = 0;
    // The persistent DIB keeps the last presented frame: the background is
    // NOT cleared per frame. A full redraw (letterbox + every block) happens
    // only when geometry/scale changes or a repaint wipes the surface.
    bool fullBlitNeeded_ = true;
    int prevScale_ = 0;
    int prevOffX_ = -1;
    int prevOffY_ = -1;

    void markAllBlocksDirty() noexcept;
#endif
public:
    // Force the next blit to redraw the whole surface (used after repaints).
    void requestFullBlit() noexcept { fullBlitNeeded_ = true; }
private:
    int width_ = 64;
    int height_ = 64;
    bool running_ = false;
    bool smooth_ = false;
    std::vector<uint32_t> framebuffer_;
    std::vector<uint32_t> palette_;
    bool keys_[256]{};
    int mouseX_ = -1;
    int mouseY_ = -1;
    bool mouseLeft_ = false;
    bool mouseRight_ = false;
    bool focused_ = false;
    int cursorHiddenCount_ = 0;
};
