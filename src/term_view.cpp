#include "term_view.hpp"
#include "theme.hpp"
#include "win_util.hpp"

#include <algorithm>

#ifndef GET_WHEEL_DELTA_WPARAM
#define GET_WHEEL_DELTA_WPARAM(wParam) ((short)HIWORD(wParam))
#endif

bool TermView::create(HWND parent, HFONT font) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = &TermView::proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "6502TermView";
    wc.hCursor = LoadCursorA(nullptr, IDC_IBEAM);
    RegisterClassA(&wc);
    font_ = font;
    hwnd_ = CreateWindowExA(0, "6502TermView", nullptr, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS,
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
    return true;
}

void TermView::setFont(HFONT font) {
    font_ = font;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void TermView::clear() {
    lines_.clear();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::string TermView::lastLineText() const {
    return lines_.empty() ? std::string{} : lines_.back().text;
}

void TermView::append(Role role, const std::string &text) {
    std::string current = lines_.empty() ? std::string{} : std::string{};
    (void)current;
    std::string pending;
    for (char c : text) {
        if (c == '\n') {
            lines_.push_back({role, pending});
            pending.clear();
            if (lines_.size() > 4000) lines_.pop_front();
        } else if (c != '\r') {
            pending += c;
        }
    }
    if (!pending.empty()) {
        if (!lines_.empty() && lines_.back().role == role && !lines_.back().text.empty() &&
            lines_.back().text.compare(0, lines_.back().text.size(), "") == 0) {
            // append to last line when no newline terminated it previously
        }
        lines_.push_back({role, pending});
        if (lines_.size() > 4000) lines_.pop_front();
    }
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int rows = rowsVisible(static_cast<int>(rc.bottom - rc.top));
    si.nMin = 0;
    si.nMax = static_cast<int>(lines_.size());
    si.nPage = static_cast<UINT>(rows);
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
    stickToBottom_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

COLORREF TermView::roleColor(Role role) {
    const Theme &t = theme();
    switch (role) {
    case Role::Echo:
        return t.accent;
    case Role::Ok:
        return t.ok;
    case Role::Warn:
        return t.warn;
    case Role::Err:
        return t.err;
    case Role::Dim:
        return t.dimFg;
    default:
        return t.fg;
    }
}

int TermView::rowsVisible(int height) const noexcept {
    return std::max(1, height / std::max(1, lineH_));
}

LRESULT CALLBACK TermView::proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTA *>(lParam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        // make hwnd_ valid before any message handler runs: handlers pass hwnd_
        // to DefWindowProcA / InvalidateRect / BeginPaint etc.
        reinterpret_cast<TermView *>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto *self = reinterpret_cast<TermView *>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
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
    case WM_VSCROLL:
    case WM_MOUSEWHEEL: {
        int delta = 0;
        if (msg == WM_MOUSEWHEEL)
            delta = -(GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA) * 3;
        else
            switch (LOWORD(wParam)) {
            case SB_LINEUP:
                delta = -1;
                break;
            case SB_LINEDOWN:
                delta = 1;
                break;
            default:
                break;
            }
        if (delta != 0) SendMessageA(GetParent(hwnd), WM_APP + 44, 0, static_cast<LPARAM>(delta));
        return 0;
    }
    default:
        return self->handleMessage(msg, wParam, lParam);
    }
}

void TermView::paintBuffer(HDC buffer, const RECT &full) {
    const Theme &t = theme();
    winutil::fillRectColor(buffer, full, t.panelBg); // serial follows the active theme
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int rows = rowsVisible(static_cast<int>(rc.bottom - rc.top));

    HFONT old = static_cast<HFONT>(SelectObject(buffer, font_));
    SetBkMode(buffer, TRANSPARENT);

    int scrollOffset = 0;
    if (!stickToBottom_) {
        scrollOffset = 0;
    }

    const int total = static_cast<int>(lines_.size());
    const int firstLine = std::max(0, total - rows + scrollOffset);
    for (int r = 0; r < rows; ++r) {
        const int idx = firstLine + r;
        if (idx >= total) break;
        const Line &line = lines_[static_cast<std::size_t>(idx)];
        winutil::drawTextOut(buffer, 6, r * lineH_ + 1, line.text, roleColor(line.role));
    }
    SelectObject(buffer, old);

    RECT border{full.right - 1, 0, full.right, full.bottom};
    winutil::fillRectColor(buffer, border, t.border);
}

void TermView::onPaint(HDC dc, const RECT &rcPaint) {
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

LRESULT TermView::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcA(hwnd_, msg, wParam, lParam);
}
