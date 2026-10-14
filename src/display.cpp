#include "display.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) static_cast<int>(static_cast<short>(LOWORD(lp)))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) static_cast<int>(static_cast<short>(HIWORD(lp)))
#endif

// SSE2 row replication for integer upscale (item 56). Output is identical to
// the scalar path; the runtime check gates machines without SSE2.
#if defined(__SSE2__) || defined(_M_X64) || defined(_MSC_VER)
#define DISP_HAS_SSE2 1
#include <emmintrin.h>
#endif

#ifdef _WIN32

namespace {

bool cpuHasSse2() {
#if defined(DISP_HAS_SSE2)
    static const bool ok = IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE) != 0;
    return ok;
#else
    return false;
#endif
}

// Nearest-neighbor row replication: dst[x] = src[x / scale] for x < dstCount.
void replicateRow(uint32_t *dst, int dstCount, const uint32_t *src, int srcCount, int scale) {
    if (scale <= 1) {
        std::memcpy(dst, src, static_cast<std::size_t>(std::min(dstCount, srcCount)) * sizeof(uint32_t));
        return;
    }
#if defined(DISP_HAS_SSE2)
    if (cpuHasSse2()) {
        // Fast path: 4 source pixels per iteration; each pixel broadcast via
        // _mm_shuffle_epi32 then stored `scale` times in 16-byte chunks. Only
        // used when scale is a multiple of 4 so every store stays strictly
        // inside its own pixel slot (no overrun past the row).
        const int groups = (scale % 4 == 0) ? std::min(srcCount / 4, dstCount / (scale * 4)) : 0;
        for (int g = 0; g < groups; ++g) {
            const __m128i px = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + g * 4));
            const __m128i p[4] = {_mm_shuffle_epi32(px, _MM_SHUFFLE(0, 0, 0, 0)),
                                  _mm_shuffle_epi32(px, _MM_SHUFFLE(1, 1, 1, 1)),
                                  _mm_shuffle_epi32(px, _MM_SHUFFLE(2, 2, 2, 2)),
                                  _mm_shuffle_epi32(px, _MM_SHUFFLE(3, 3, 3, 3))};
            uint32_t *d = dst + g * 4 * scale;
            for (int j = 0; j < 4; ++j)
                for (int s = 0; s + 4 <= scale; s += 4)
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(d + j * scale + s), p[j]);
        }
        // General path: per-pixel broadcast; stores are clamped so nothing
        // ever lands past dstCount, valid for any scale >= 2.
        int pos = groups * 4 * scale;
        for (int x = groups * 4; x < srcCount && pos < dstCount; ++x) {
            const __m128i pv = _mm_set1_epi32(src[x]);
            int s = 0;
            for (; s + 4 <= scale && pos + s + 4 <= dstCount; s += 4)
                _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + pos + s), pv);
            for (; s < scale && pos < dstCount; ++s) dst[pos + s] = src[x];
            pos += scale;
        }
        return;
    }
#endif
    int pos = 0;
    for (int x = 0; x < srcCount && pos < dstCount; ++x)
        for (int s = 0; s < scale && pos < dstCount; ++s) dst[pos++] = src[x];
}

} // namespace

Display::Display() {
    resetPalette();
    focused_ = false;
    cursorHiddenCount_ = 0;
}

Display::~Display() {
    destroy();
}

void Display::resetPalette() {
    palette_.assign(256, 0xFF000000u);
    for (int i = 0; i < 256; ++i) {
        const int level = (i * 255) / 255;
        const uint8_t r = static_cast<uint8_t>(level);
        const uint8_t g = static_cast<uint8_t>(level);
        const uint8_t b = static_cast<uint8_t>(level);
        palette_[static_cast<std::size_t>(i)] =
            0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
    }
    for (int i = 1; i < 16; ++i) {
        const uint32_t hue = (static_cast<uint32_t>(i * 17) << 16) |
                             (static_cast<uint32_t>(255 - i * 12) << 8) | static_cast<uint32_t>(60 + i * 10);
        palette_[static_cast<std::size_t>(i)] = 0xFF000000u | hue;
    }
    palette_[0] = 0xFF101018;
    palette_[15] = 0xFFF0F0F8;
    ++paletteVersion_; // cached source bytes must be re-rendered
}

void Display::setPaletteEntry(int index, uint32_t argb) {
    if (index >= 0 && index < 256) {
        palette_[static_cast<std::size_t>(index)] = argb | 0xFF000000u;
        ++paletteVersion_;
        if (!blockDirty_.empty()) markAllBlocksDirty();
    }
}

uint32_t Display::paletteEntry(int index) const noexcept {
    return (index >= 0 && index < 256) ? palette_[static_cast<std::size_t>(index)] : 0;
}

bool Display::createChild(HWND parent, int width, int height) {
    destroy();
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    framebuffer_.assign(static_cast<std::size_t>(width_) * height_, 0xFF000000u);
    static bool registered = false;
    if (!registered) {
        WNDCLASSA wc{};
        wc.lpfnWndProc = Display::wndProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "6502Screen";
        wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(6, 6, 10));
        RegisterClassA(&wc);
        registered = true;
    }
    hwnd_ = CreateWindowExA(WS_EX_CLIENTEDGE, "6502Screen", "6502 Screen",
                            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP, 0, 0, width_, height_, parent,
                            nullptr, GetModuleHandleA(nullptr), this);
    if (!hwnd_) return false;
    running_ = true;
    fullBlitNeeded_ = true;
    return true;
}

void Display::destroy() {
    if (bitmap_) {
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
    }
    if (memoryDc_) {
        DeleteDC(memoryDc_);
        memoryDc_ = nullptr;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    bitmapBits_ = nullptr;
    dibWidth_ = dibHeight_ = 0;
    framebuffer_.clear();
    srcCache_.clear();
    blockDirty_.clear();
    blockCols_ = blockRows_ = 0;
    srcCacheValid_ = false;
    prevDibW_ = prevDibH_ = 0;
    fullBlitNeeded_ = true;
    prevScale_ = 0;
    prevOffX_ = prevOffY_ = -1;
    running_ = false;
}

void Display::markAllBlocksDirty() noexcept {
    std::fill(blockDirty_.begin(), blockDirty_.end(), static_cast<uint8_t>(1));
}

void Display::resize(int width, int height) {
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    framebuffer_.assign(static_cast<std::size_t>(width_) * height_, 0xFF000000u);
    srcCache_.assign(static_cast<std::size_t>(width_) * height_, 0);
    blockCols_ = (width_ + kBlockPx - 1) / kBlockPx;
    blockRows_ = (height_ + kBlockPx - 1) / kBlockPx;
    blockDirty_.assign(static_cast<std::size_t>(blockCols_) * blockRows_, 1); // full copy
    srcCacheValid_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Display::clear(uint32_t argb) {
    std::fill(framebuffer_.begin(), framebuffer_.end(), argb);
    markAllBlocksDirty(); // clear() marks all dirty
    srcCacheValid_ = false; // next renderMemory re-renders everything
    fullBlitNeeded_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Display::renderMemory(const uint8_t *memory, uint16_t baseAddress, int sourceWidth, int sourceHeight) {
    if (!memory || sourceWidth <= 0 || sourceHeight <= 0 || framebuffer_.empty()) return;
    const int w = std::min(width_, sourceWidth);
    const int h = std::min(height_, sourceHeight);

    // (Re)build the dirty-block grid when the geometry changes.
    const int cols = (width_ + kBlockPx - 1) / kBlockPx;
    const int rows = (height_ + kBlockPx - 1) / kBlockPx;
    if (static_cast<int>(srcCache_.size()) != width_ * height_ || blockCols_ != cols ||
        blockRows_ != rows) {
        srcCache_.assign(static_cast<std::size_t>(width_) * height_, 0);
        blockCols_ = cols;
        blockRows_ = rows;
        blockDirty_.assign(static_cast<std::size_t>(cols) * rows, 1);
        srcCacheValid_ = false;
    }
    const bool paletteChanged = cachePaletteVersion_ != paletteVersion_;

    uint32_t *dst = framebuffer_.data();
    for (int by = 0; by < blockRows_; ++by) {
        for (int bx = 0; bx < blockCols_; ++bx) {
            const int x0 = bx * kBlockPx;
            const int y0 = by * kBlockPx;
            const int x1 = std::min(x0 + kBlockPx, w);
            const int y1 = std::min(y0 + kBlockPx, h);
            if (x1 <= x0 || y1 <= y0) continue;

            // Compare incoming tile bytes against the cached copy.
            bool changed = !srcCacheValid_ || paletteChanged;
            if (!changed) {
                for (int y = y0; y < y1 && !changed; ++y) {
                    int si = baseAddress + y * sourceWidth;
                    const std::size_t ci = static_cast<std::size_t>(y) * width_;
                    for (int x = x0; x < x1; ++x)
                        if (srcCache_[ci + x] != memory[static_cast<uint16_t>(si + x)]) {
                            changed = true;
                            break;
                        }
                }
            }
            if (!changed) continue;

            for (int y = y0; y < y1; ++y) {
                int si = baseAddress + y * sourceWidth;
                uint32_t *row = dst + static_cast<std::size_t>(y) * width_;
                const std::size_t ci = static_cast<std::size_t>(y) * width_;
                for (int x = x0; x < x1; ++x) {
                    const uint8_t v = memory[static_cast<uint16_t>(si + x)];
                    srcCache_[ci + x] = v;
                    row[x] = palette_[v];
                }
            }
            blockDirty_[static_cast<std::size_t>(by) * blockCols_ + bx] = 1;
        }
    }
    srcCacheValid_ = true;
    cachePaletteVersion_ = paletteVersion_;
    blit();
}

void Display::ensureDib(HDC dc) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int cw = std::max(1, static_cast<int>(rc.right - rc.left));
    const int ch = std::max(1, static_cast<int>(rc.bottom - rc.top));
    if (bitmap_ && memoryDc_ && dibWidth_ == cw && dibHeight_ == ch) return;
    if (bitmap_) DeleteObject(bitmap_);
    if (memoryDc_) DeleteDC(memoryDc_);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = cw;
    bmi.bmiHeader.biHeight = -ch;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bitmap_ = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bitmapBits_, nullptr, 0);
    memoryDc_ = CreateCompatibleDC(dc);
    SelectObject(memoryDc_, bitmap_);
    dibWidth_ = cw;
    dibHeight_ = ch;
}

void Display::confineMouse() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT *>(&rc), 2);
    ClipCursor(&rc);
}

void Display::releaseMouse() {
    ClipCursor(nullptr);
}

// Focus mode must be perfectly balanced: every enter pairs with exactly one
// cursor hide, and every exit (ESC, alt-tab, kill focus, destroy) restores
// the pointer. Keeping the bookkeeping in one place prevents the "escaped but
// still trapped / cursor visible" desync.
void Display::enterFocusMode() {
    if (!hwnd_ || focused_) return;
    focused_ = true;
    SetFocus(hwnd_);
    if (cursorHiddenCount_ == 0) {
        ShowCursor(FALSE);
        cursorHiddenCount_ = 1;
    }
    confineMouse();
}

void Display::exitFocusMode(bool refocusParent) {
    const bool wasFocused = focused_;
    focused_ = false;
    if (wasFocused && refocusParent && hwnd_) SetFocus(GetParent(hwnd_));
    ReleaseCapture();
    releaseMouse();
    while (cursorHiddenCount_ > 0) {
        ShowCursor(TRUE);
        --cursorHiddenCount_;
    }
}

void Display::blit() {
    if (!running_ || !hwnd_ || !bitmapBits_) return;
    HDC dc = GetDC(hwnd_);
    if (!dc) return;
    ensureDib(dc);

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;

    // A resized DIB invalidates every cached block: full copy on resize.
    if (dibWidth_ != prevDibW_ || dibHeight_ != prevDibH_) {
        markAllBlocksDirty();
        fullBlitNeeded_ = true;
        prevDibW_ = dibWidth_;
        prevDibH_ = dibHeight_;
    }

    if (cw > 0 && ch > 0 && !framebuffer_.empty()) {
        int scale = std::max(1, std::min(cw / width_, ch / height_));
        const int drawW = std::min(cw, width_ * scale);
        const int drawH = std::min(ch, height_ * scale);
        scale = std::max(1, std::min(drawW / width_, drawH / height_));
        const int outW = width_ * scale;
        const int outH = height_ * scale;
        const int offX = (cw - outW) / 2;
        const int offY = (ch - outH) / 2;

        // Geometry change (resize/layout): repaint letterbox + everything.
        if (fullBlitNeeded_ || scale != prevScale_ || offX != prevOffX_ || offY != prevOffY_) {
            HBRUSH bg = CreateSolidBrush(theme().windowBg); // letterbox follows the active theme
            FillRect(memoryDc_, &rc, bg);
            DeleteObject(bg);
            markAllBlocksDirty();
            fullBlitNeeded_ = false;
            prevScale_ = scale;
            prevOffX_ = offX;
            prevOffY_ = offY;
        }

        const uint32_t *src = framebuffer_.data();
        auto *dstBits = static_cast<uint32_t *>(bitmapBits_);
        const int stride = dibWidth_;

        if (smooth_) {
            // StretchDIBits needs a full BITMAPINFO to interpret the source
            // bits; passing nullptr made the call fail silently and left the
            // screen permanently black when smooth scaling was enabled.
            BITMAPINFO srcBmi{};
            srcBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            srcBmi.bmiHeader.biWidth = width_;
            srcBmi.bmiHeader.biHeight = -height_; // top-down rows
            srcBmi.bmiHeader.biPlanes = 1;
            srcBmi.bmiHeader.biBitCount = 32;
            srcBmi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(memoryDc_, HALFTONE);
            StretchDIBits(memoryDc_, offX, offY, outW, outH, 0, 0, width_, height_, src, &srcBmi,
                          DIB_RGB_COLORS, SRCCOPY);
            // Full-surface path: everything is on screen, mark clean.
            std::fill(blockDirty_.begin(), blockDirty_.end(), static_cast<uint8_t>(0));
            fullBlitNeeded_ = false;
        } else {
            if (blockDirty_.size() != static_cast<std::size_t>(blockCols_) * blockRows_)
                markAllBlocksDirty();
            // Copy only dirty source blocks; mark clean after each blit.
            for (int by = 0; by < blockRows_; ++by) {
                for (int bx = 0; bx < blockCols_; ++bx) {
                    uint8_t &flag = blockDirty_[static_cast<std::size_t>(by) * blockCols_ + bx];
                    if (!flag) continue;
                    flag = 0;
                    const int sx = bx * kBlockPx;
                    const int sy = by * kBlockPx;
                    const int sw = std::min(kBlockPx, width_ - sx);
                    const int sh = std::min(kBlockPx, height_ - sy);
                    if (sw <= 0 || sh <= 0) continue;
                    int dx = offX + sx * scale;
                    const int dy = offY + sy * scale;
                    int dw = sw * scale;
                    const int dh = sh * scale;
                    if (dx >= dibWidth_ || dy >= dibHeight_) continue;
                    dw = std::min(dw, dibWidth_ - dx);
                    if (dw <= 0) continue;
                    const int rowLimit = std::min(dh, dibHeight_ - dy);
                    for (int y = 0; y < rowLimit; ++y) {
                        const int srcY = sy + y / scale;
                        if (srcY < 0 || srcY >= height_) continue;
                        replicateRow(dstBits + static_cast<std::size_t>(dy + y) * stride + dx, dw,
                                     src + static_cast<std::size_t>(srcY) * width_ + sx, sw, scale);
                    }
                }
            }
        }
    }

    BeginPath(memoryDc_);
    BitBlt(dc, 0, 0, dibWidth_, dibHeight_, memoryDc_, 0, 0, SRCCOPY);
    EndPath(memoryDc_);
    ReleaseDC(hwnd_, dc);
}

bool Display::keyDown(int virtualKey) const noexcept {
    return virtualKey >= 0 && virtualKey < 256 && keys_[virtualKey];
}

bool Display::saveScreenshot(const std::string &path, std::string &error) const {
    if (framebuffer_.empty()) {
        error = "nothing to save";
        return false;
    }
    const int w = width_;
    const int h = height_;
    std::vector<uint8_t> pixels(static_cast<std::size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t px = framebuffer_[static_cast<std::size_t>((h - 1 - y)) * w + x];
            const std::size_t o = (static_cast<std::size_t>(y) * w + x) * 3;
            pixels[o + 0] = static_cast<uint8_t>(px & 0xFF);
            pixels[o + 1] = static_cast<uint8_t>((px >> 8) & 0xFF);
            pixels[o + 2] = static_cast<uint8_t>((px >> 16) & 0xFF);
        }
    }
    const std::size_t rowSize = (static_cast<std::size_t>(w) * 3 + 3) & ~std::size_t{3};
    const std::size_t dataSize = rowSize * h;
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot create " + path;
        return false;
    }
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = static_cast<DWORD>(fh.bfOffBits + dataSize);
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = static_cast<DWORD>(dataSize);
    out.write(reinterpret_cast<const char *>(&fh), sizeof(fh));
    out.write(reinterpret_cast<const char *>(&ih), sizeof(ih));
    std::vector<uint8_t> row(static_cast<std::size_t>(rowSize), 0);
    for (int y = 0; y < h; ++y) {
        std::memcpy(row.data(), &pixels[static_cast<std::size_t>(y) * w * 3],
                    static_cast<std::size_t>(w) * 3);
        out.write(reinterpret_cast<const char *>(row.data()), static_cast<std::streamsize>(rowSize));
    }
    if (!out) {
        error = "write failed";
        return false;
    }
    error = "screenshot saved: " + path;
    return true;
}

LRESULT CALLBACK Display::wndProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    Display *self = reinterpret_cast<Display *>(GetWindowLongPtrA(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTA *>(lParam);
        self = reinterpret_cast<Display *>(cs->lpCreateParams);
        SetWindowLongPtrA(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        if (!self) return 0;
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (dc) self->ensureDib(dc);
        EndPaint(h, &ps);
        // Windows may have wiped the surface (occlusion/expose): present the
        // full frame through the normal scaled path instead of a raw copy.
        if (self->running_) {
            self->requestFullBlit();
            self->blit();
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (self) {
            if (wParam < 256) self->keys_[wParam] = true;
            if (wParam == VK_ESCAPE && self->focused_) self->exitFocusMode(true);
        }
        break;
    case WM_KEYUP:
        if (self && wParam < 256) self->keys_[wParam] = false;
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS;
    case WM_MOUSEMOVE: {
        if (!self) break;
        RECT rc{};
        GetClientRect(h, &rc);
        const int cw = std::max(1, static_cast<int>(rc.right - rc.left));
        const int chh = std::max(1, static_cast<int>(rc.bottom - rc.top));
        int scale = std::max(1, std::min(cw / self->width_, chh / self->height_));
        const int outW = self->width_ * scale;
        const int outH = self->height_ * scale;
        const int offX = (cw - outW) / 2;
        const int offY = (chh - outH) / 2;
        const int mx = (GET_X_LPARAM(lParam) - offX) / scale;
        const int my = (GET_Y_LPARAM(lParam) - offY) / scale;
        self->mouseX_ = (mx >= 0 && mx < self->width_) ? mx : -1;
        self->mouseY_ = (my >= 0 && my < self->height_) ? my : -1;
        break;
    }
    case WM_LBUTTONDOWN:
        if (self) {
            self->mouseLeft_ = true;
            SetCapture(h);
            self->enterFocusMode();
        }
        break;
    case WM_LBUTTONUP:
        if (self) {
            self->mouseLeft_ = false;
            ReleaseCapture();
        }
        break;
    case WM_RBUTTONDOWN:
        if (self) {
            self->mouseRight_ = true;
            SetCapture(h);
        }
        break;
    case WM_RBUTTONUP:
        if (self) {
            self->mouseRight_ = false;
            ReleaseCapture();
        }
        break;
    case WM_SETFOCUS:
        if (self && self->focused_ && self->cursorHiddenCount_ == 0) {
            ShowCursor(FALSE);
            self->cursorHiddenCount_ = 1;
        }
        if (self && self->focused_) self->confineMouse();
        break;
    case WM_KILLFOCUS:
        if (self && self->focused_) self->exitFocusMode(false);
        break;
    case WM_SIZE:
        if (self) {
            if (self->focused_) self->confineMouse(); // keep the clip rect in sync
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOVE:
    case WM_WINDOWPOSCHANGED:
        if (self && self->focused_) self->confineMouse();
        break;
    case WM_NCDESTROY:
        if (self) {
            ClipCursor(nullptr); // never leave the pointer confined
            while (self->cursorHiddenCount_ > 0) { // never leave it hidden either
                ShowCursor(TRUE);
                --self->cursorHiddenCount_;
            }
            self->running_ = false;
            self->hwnd_ = nullptr;
        }
        break;
    default:
        break;
    }
    return DefWindowProcA(h, msg, wParam, lParam);
}

#else

Display::Display() {
    resetPalette();
}
Display::~Display() = default;
bool Display::createChild(void *, int width, int height) {
    width_ = width;
    height_ = height;
    framebuffer_.assign(static_cast<std::size_t>(width) * height, 0xFF000000u);
    running_ = true;
    return true;
}
void Display::destroy() {
    running_ = false;
    framebuffer_.clear();
}
void Display::resize(int width, int height) {
    width_ = width;
    height_ = height;
    framebuffer_.assign(static_cast<std::size_t>(width) * height, 0xFF000000u);
}
void Display::renderMemory(const uint8_t *, uint16_t, int, int) {}
void Display::clear(uint32_t argb) {
    std::fill(framebuffer_.begin(), framebuffer_.end(), argb);
}
bool Display::keyDown(int) const noexcept {
    return false;
}
void Display::resetPalette() {
    palette_.assign(256, 0xFF202020u);
}
void Display::setPaletteEntry(int index, uint32_t argb) {
    if (index >= 0 && index < 256) palette_[index] = argb;
}
uint32_t Display::paletteEntry(int index) const noexcept {
    return (index >= 0 && index < 256) ? palette_[index] : 0;
}
bool Display::saveScreenshot(const std::string &, std::string &error) const {
    error = "screenshots are Windows-only";
    return false;
}

#endif
