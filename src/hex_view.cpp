#include "hex_view.hpp"
#include "memory.hpp"
#include "theme.hpp"
#include "win_util.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) static_cast<int>(static_cast<short>(LOWORD(lp)))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) static_cast<int>(static_cast<short>(HIWORD(lp)))
#endif

namespace {

// Dark-blue -> cyan -> yellow -> red ramp for the access heatmap (item 46).
COLORREF heatColor(int v) {
    v = std::clamp(v, 0, 255);
    double t;
    int c0[3], c1[3];
    if (v < 85) {
        t = v / 85.0;
        const int a[3] = {8, 10, 64}, b[3] = {0, 190, 220};
        std::memcpy(c0, a, sizeof(a));
        std::memcpy(c1, b, sizeof(b));
    } else if (v < 170) {
        t = (v - 85) / 85.0;
        const int a[3] = {0, 190, 220}, b[3] = {235, 215, 60};
        std::memcpy(c0, a, sizeof(a));
        std::memcpy(c1, b, sizeof(b));
    } else {
        t = (v - 170) / 85.0;
        const int a[3] = {235, 215, 60}, b[3] = {225, 45, 35};
        std::memcpy(c0, a, sizeof(a));
        std::memcpy(c1, b, sizeof(b));
    }
    const auto mix = [&](int ch) { return static_cast<int>(c0[ch] + (c1[ch] - c0[ch]) * t); };
    return RGB(mix(0), mix(1), mix(2));
}

#ifndef TPM_NOACTIVATE
#define TPM_NOACTIVATE 0x0100L
#endif

const std::array<COLORREF, 256> &heatTable() {
    static const std::array<COLORREF, 256> table = [] {
        std::array<COLORREF, 256> t{};
        for (int i = 0; i < 256; ++i) t[static_cast<std::size_t>(i)] = heatColor(i);
        return t;
    }();
    return table;
}

} // namespace

void HexView::setHeatmap(bool on) noexcept {
    heatmap_ = on;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool HexView::create(HWND parent, HFONT font) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = &HexView::proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "6502HexView";
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    font_ = font;
    hwnd_ = CreateWindowExA(0, "6502HexView", nullptr, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS,
                            0, 0, 300, 200, parent, nullptr, GetModuleHandleA(nullptr), this);
    if (!hwnd_) return false;
    HDC dc = GetDC(hwnd_);
    HFONT old = static_cast<HFONT>(SelectObject(dc, font_));
    TEXTMETRICA tm{};
    GetTextMetricsA(dc, &tm);
    SIZE sz{};
    GetTextExtentPoint32A(dc, "M", 1, &sz);
    charW_ = std::max(4, static_cast<int>(sz.cx));
    lineH_ = tm.tmHeight + 3;
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    updateScrollbar();
    return true;
}

void HexView::setFont(HFONT font) {
    font_ = font;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK HexView::proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTA *>(lParam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        // make hwnd_ valid before any message handler runs: handlers pass hwnd_
        // to DefWindowProcA / InvalidateRect / BeginPaint etc.
        reinterpret_cast<HexView *>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto *self = reinterpret_cast<HexView *>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcA(hwnd, msg, wParam, lParam);
    if (!self->hwnd_) self->hwnd_ = hwnd;

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        self->onPaint(dc, ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        self->updateScrollbar();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_VSCROLL: {
        const int action = LOWORD(wParam);
        int rows = self->rowsVisible(1000);
        switch (action) {
        case SB_LINEUP:
            self->topRow_ -= 1;
            break;
        case SB_LINEDOWN:
            self->topRow_ += 1;
            break;
        case SB_PAGEUP:
            self->topRow_ -= rows;
            break;
        case SB_PAGEDOWN:
            self->topRow_ += rows;
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            self->topRow_ = HIWORD(wParam);
            break;
        default:
            break;
        }
        if (self->follow_ != Follow::Free) self->follow_ = Follow::Free;
        self->topRow_ = std::clamp(self->topRow_, 0, 0x10000 / 16 - 1);
        self->updateScrollbar();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        self->topRow_ -= (delta / WHEEL_DELTA) * 3;
        self->topRow_ = std::clamp(self->topRow_, 0, 0x10000 / 16 - 1);
        self->updateScrollbar();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        if (self->memory_) {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const int row = self->topRow_ + std::max(0, y / self->lineH_);
            const int col = (x - 6 * self->charW_ - 8) / (3 * self->charW_);
            if (row >= 0 && row < 0x10000 / 16 && col >= 0 && col < 16) {
                self->selected_ = static_cast<uint16_t>(row * 16 + col);
                self->nibblePhase_ = -1;
                SendMessageA(GetParent(hwnd), WM_APP + 41, 2, 0);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    default:
        return self->handleMessage(msg, wParam, lParam);
    }
}

int HexView::rowsVisible(int height) const noexcept {
    return std::max(1, height / std::max(1, lineH_));
}

void HexView::updateScrollbar() {
    RECT rc{};
    if (!GetClientRect(hwnd_, &rc)) return;
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = 0x10000 / 16;
    si.nPage = static_cast<UINT>(std::max<LONG>(1, rc.bottom / lineH_));
    si.nPos = topRow_;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void HexView::setFollow(Follow mode, uint16_t custom) {
    follow_ = mode;
    customBase_ = custom;
    syncFollow();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void HexView::jumpTo(uint16_t address) {
    topRow_ = std::max(0, static_cast<int>((address & 0xFFF0u) / 16) - rowsVisible(400) / 4);
    selected_ = address;
    updateScrollbar();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void HexView::syncFollow() {
    topRow_ = std::max(0, static_cast<int>(customBase_ / 16) - rowsVisible(400) / 4);
    updateScrollbar();
}

void HexView::refresh() {
    if (!memory_) return;
    if (memory_->pageChangedSince(topRow_ * 16, static_cast<std::size_t>(rowsVisible(600)) * 16 + 32,
                                  lastEpoch_)) {
        lastEpoch_ = memory_->epoch();
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else {
        lastEpoch_ = memory_->epoch();
    }
}

void HexView::paintBuffer(HDC buffer, const RECT &full) {
    const Theme &t = theme();
    winutil::fillRectColor(buffer, full, t.panelBg);
    if (!memory_) return;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int rows = rowsVisible(static_cast<int>(rc.bottom - rc.top)) + 1;
    HFONT oldFont = static_cast<HFONT>(SelectObject(buffer, font_));
    SetBkMode(buffer, TRANSPARENT);

    char addr[8];
    char byte[8];
    for (int r = 0; r < rows; ++r) {
        const int row = topRow_ + r;
        if (row >= 0x10000 / 16) break;
        const int y = r * lineH_;
        std::snprintf(addr, sizeof(addr), "%04X", row * 16);
        SetTextColor(buffer, t.hexAddr);
        TextOutA(buffer, 6, y, addr, 4);

        for (int c = 0; c < 16; ++c) {
            const uint16_t a = static_cast<uint16_t>(row * 16 + c);
            const uint8_t v = memory_->read(a);
            std::snprintf(byte, sizeof(byte), "%02X", v);
            const int bx = 6 * charW_ + 8 + c * 3 * charW_;
            const bool isSel = a == selected_;
            const bool changed = memory_->pageChangedSince(a, 1, lastEpoch_ == 0 ? 0 : lastEpoch_ - 4096);
            COLORREF fg;
            if (isSel) {
                RECT selRc{bx - 2, y, bx + 2 * charW_ + 2, y + lineH_};
                winutil::fillRectColor(buffer, selRc, t.selectionBg);
            }
            bool heated = false;
            if (heatmap_) {
                const int heat = memory_->heat(a);
                if (heat > 0) {
                    RECT cell{bx - 1, y, bx + 2 * charW_ + 1, y + lineH_ - 1};
                    COLORREF hc = heatTable()[static_cast<std::size_t>(std::clamp(heat, 0, 255))];
                    if (isSel) hc = blend(t.selectionBg, hc, 0.6); // keep selection visible
                    winutil::fillRectColor(buffer, cell, hc);
                    // Contrast text so hot cells stay readable.
                    const int r = static_cast<int>(hc & 0xFFu), g = static_cast<int>((hc >> 8) & 0xFFu),
                              b = static_cast<int>((hc >> 16) & 0xFFu);
                    fg = (r * 299 + g * 587 + b * 114) / 1000 >= 128 ? RGB(12, 12, 14) : RGB(240, 240, 244);
                    heated = true;
                }
            }
            if (!heated)
                fg = changed && !isSel ? t.hexChanged : (isSel ? t.selectionFg : t.fg);
            SetTextColor(buffer, fg);
            TextOutA(buffer, bx, y, byte, 2);
        }

        const int ax = 6 * charW_ + 8 + 16 * 3 * charW_ + 10;
        for (int c = 0; c < 16; ++c) {
            const uint8_t v = memory_->read(static_cast<uint16_t>(row * 16 + c));
            char ch[2] = {static_cast<char>(v >= 32 && v < 127 ? v : '.'), 0};
            SetTextColor(buffer, t.hexAscii);
            TextOutA(buffer, ax + c * charW_, y, ch, 1);
        }
    }

    if (heatmap_) drawLegend(buffer, rc);
    SelectObject(buffer, oldFont);
}

void HexView::drawLegend(HDC buffer, const RECT &rc) {
    // Tiny gradient strip along the last row: cold (0) -> hot (255).
    const int stripH = 5;
    RECT strip{6, rc.bottom - stripH - 2, std::max(6 + 32, static_cast<int>(rc.right) - 6),
               rc.bottom - 2};
    const int cols = std::max(1, static_cast<int>(strip.right - strip.left));
    for (int i = 0; i < cols; ++i) {
        RECT col{strip.left + i, strip.top, strip.left + i + 1, strip.bottom};
        winutil::fillRectColor(buffer, col,
                               heatTable()[static_cast<std::size_t>((i * 255) / (cols - 1 > 0 ? cols - 1 : 1))]);
    }
    winutil::frameRectColor(buffer, strip, theme().border);
}

void HexView::onPaint(HDC dc, const RECT &rcPaint) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1, static_cast<int>(rc.right)),
                                         std::max(1, static_cast<int>(rc.bottom)));
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(mem, bmp));
    paintBuffer(mem, rc);
    BitBlt(dc, rcPaint.left, rcPaint.top, rcPaint.right - rcPaint.left, rcPaint.bottom - rcPaint.top, mem,
           rcPaint.left, rcPaint.top, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

LRESULT HexView::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CHAR) {
        const char ch = static_cast<char>(wParam);
        if (ch == 'h' || ch == 'H') { // heatmap toggle when focused
            setHeatmap(!heatmap_);
            return 0;
        }
        const int nibble = (ch >= '0' && ch <= '9')   ? ch - '0'
                           : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
                           : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10
                                                      : -1;
        if (nibble < 0 || !memory_) return 0;
        if (nibblePhase_ < 0 || nibblePhase_ != selected_) {
            const uint8_t cur = memory_->read(selected_);
            const_cast<Memory *>(memory_)->writeRaw(selected_,
                                                    static_cast<uint8_t>((cur & 0x0Fu) | (nibble << 4)));
            nibblePhase_ = selected_;
        } else {
            const uint8_t cur = memory_->read(selected_);
            const_cast<Memory *>(memory_)->writeRaw(selected_, static_cast<uint8_t>((cur & 0xF0u) | nibble));
            nibblePhase_ = -1;
            selected_ = static_cast<uint16_t>(selected_ + 1);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    if (msg == WM_RBUTTONUP) { // context menu: heatmap toggle
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hwnd_, &pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuA(menu, MF_STRING | (heatmap_ ? MF_CHECKED : MF_UNCHECKED), 1, "Heatmap\tH");
        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NOACTIVATE, pt.x, pt.y,
                                       0, hwnd_, nullptr);
        DestroyMenu(menu);
        if (cmd == 1) setHeatmap(!heatmap_);
        return 0;
    }
    return DefWindowProcA(hwnd_, msg, wParam, lParam);
}
