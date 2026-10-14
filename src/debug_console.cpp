#include "debug_console.hpp"
#include "serial.hpp"
#include "settings.hpp"
#include "theme.hpp"
#include "win_util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef GET_WHEEL_DELTA_WPARAM
#define GET_WHEEL_DELTA_WPARAM(wParam) ((short)HIWORD(wParam))
#endif

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) static_cast<int>(static_cast<short>(LOWORD(lp)))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) static_cast<int>(static_cast<short>(HIWORD(lp)))
#endif

namespace {

constexpr int kHeaderH = 22;

enum ButtonId { kBtnMode = 0, kBtnLog, kBtnPort, kBtnCount };

// Shared layout so painting and hit-testing always agree.
void layoutButtons(const RECT &client, RECT out[kBtnCount]) {
    const int w = 52, h = kHeaderH - 6, gap = 6;
    int x = client.right - 6;
    for (int i = kBtnCount - 1; i >= 0; --i) {
        out[i] = RECT{x - w, (kHeaderH - h) / 2, x, (kHeaderH - h) / 2 + h};
        x -= w + gap;
    }
}

const char *buttonLabel(int id) {
    SerialMonitor &mon = SerialMonitor::instance();
    switch (id) {
    case kBtnMode:
        return mon.hexMode() ? "HEX" : "ASC";
    case kBtnLog:
        return mon.logging() ? "LOG*" : "LOG";
    case kBtnPort:
        return mon.isOpen() ? "CLOSE" : "OPEN";
    default:
        return "?";
    }
}

void pressButton(HWND hwnd, int id) {
    SerialMonitor &mon = SerialMonitor::instance();
    static std::string logPath = "serial_log.txt";
    switch (id) {
    case kBtnMode:
        mon.setHexMode(!mon.hexMode());
        break;
    case kBtnLog:
        if (!mon.logging())
            mon.openLog(logPath);
        else
            mon.closeLog();
        break;
    case kBtnPort: {
        if (mon.isOpen()) {
            mon.close();
        } else {
            std::string err;
            mon.open(Settings::get().comPort, Settings::get().baud, err);
        }
        break;
    }
    default:
        break;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

} // namespace

bool DebugConsole::create(HWND parent, HFONT font) {
    font_ = font;
    WNDCLASSA wc{};
    wc.lpfnWndProc = &DebugConsole::proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "6502DebugConsole";
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    hwnd_ = CreateWindowExA(WS_EX_CLIENTEDGE | WS_EX_TOPMOST, "6502DebugConsole", "Debug Console",
                            WS_POPUP | WS_VSCROLL | WS_CLIPSIBLINGS | WS_CAPTION | WS_SYSMENU |
                                WS_THICKFRAME | WS_MAXIMIZEBOX,
                            100, 100, 500, 300, parent, nullptr, GetModuleHandleA(nullptr), this);
    if (!hwnd_) return false;
    HDC dc = GetDC(hwnd_);
    HFONT old = static_cast<HFONT>(SelectObject(dc, font_));
    TEXTMETRICA tm{};
    GetTextMetricsA(dc, &tm);
    SIZE sz{};
    GetTextExtentPoint32A(dc, "M", 1, &sz);
    charW_ = std::max(4, static_cast<int>(sz.cx));
    lineH_ = tm.tmHeight + 2;
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    // Item 98: periodic receive-path polling for the serial monitor.
    SetTimer(hwnd_, 1, 100, nullptr);
    return true;
}

void DebugConsole::setFont(HFONT font) {
    font_ = font;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void DebugConsole::clear() {
    lines_.clear();
    scrollOffset_ = 0;
    selectAnchor_ = selectEnd_ = -1;
    selecting_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void DebugConsole::append(const std::string &text) {
    std::string pending;
    for (char c : text) {
        if (c == '\n') {
            lines_.push_back(pending);
            pending.clear();
            if (lines_.size() > 2000) lines_.pop_front();
        } else if (c != '\r') {
            pending += c;
        }
    }
    if (!pending.empty()) {
        lines_.push_back(pending);
        if (lines_.size() > 2000) lines_.pop_front();
    }
    stickToBottom_ = true;
    selectAnchor_ = selectEnd_ = -1;
    selecting_ = false;
    scrollOffset_ = 0;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// Receive path: drain the serial monitor and mirror new lines into the view.
// Free function because the console header is not extended (additive-only
// work lives in serial.hpp / this TU).
void pollSerialInto(DebugConsole &console) {
    SerialMonitor &mon = SerialMonitor::instance();
    mon.poll();
    static uint64_t consumed = 0;
    const uint64_t total = mon.totalLines();
    if (total == consumed) return;
    const auto &ring = mon.lines();
    std::size_t take = static_cast<std::size_t>(total - consumed);
    take = std::min(take, ring.size());
    const std::size_t first = ring.size() - take;
    for (std::size_t i = first; i < ring.size(); ++i) {
        char stamp[40];
        std::snprintf(stamp, sizeof(stamp), "[RX +%lums] ",
                      static_cast<unsigned long>(ring[i].tickMs));
        console.append(stamp + ring[i].text); // appends one line
    }
    consumed = total;
}

void DebugConsole::setVisible(bool v) {
    visible_ = v;
    if (!hwnd_) return;
    ShowWindow(hwnd_, v ? SW_SHOW : SW_HIDE);
    if (v)
        SetTimer(hwnd_, 1, 100, nullptr);
    else
        KillTimer(hwnd_, 1);
}

LRESULT CALLBACK DebugConsole::proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTA *>(lParam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        reinterpret_cast<DebugConsole *>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto *self = reinterpret_cast<DebugConsole *>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcA(hwnd, msg, wParam, lParam);
    if (!self->hwnd_) self->hwnd_ = hwnd;
    switch (msg) {
    case WM_TIMER:
        if (wParam == 1) pollSerialInto(*self);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        self->onPaint(dc, ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CHAR: {
        // Keys: H = hex/ascii mode toggle, L = toggle log file.
        const char ch = static_cast<char>(wParam);
        SerialMonitor &mon = SerialMonitor::instance();
        if (ch == 'h' || ch == 'H')
            mon.toggleHexMode();
        else if (ch == 'l' || ch == 'L') {
            static const std::string kLogPath = "serial_log.txt";
            if (!mon.logging())
                mon.openLog(kLogPath);
            else
                mon.closeLog();
        } else
            break;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && wParam == 'A') {
            if (!self->lines_.empty()) {
                self->selectAnchor_ = 0;
                self->selectEnd_ = static_cast<int>(self->lines_.size()) - 1;
            } else {
                self->selectAnchor_ = self->selectEnd_ = -1;
            }
            self->selecting_ = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (ctrl && wParam == 'C') {
            int first = -1, last = -1;
            if (self->selectAnchor_ >= 0 && self->selectEnd_ >= 0) {
                first = std::min(self->selectAnchor_, self->selectEnd_);
                last = std::max(self->selectAnchor_, self->selectEnd_);
            }
            if (first >= 0 && last >= first && last < static_cast<int>(self->lines_.size())) {
                std::string text;
                for (int i = first; i <= last; ++i) {
                    if (i != first) text += "\r\n";
                    text += self->lines_[static_cast<std::size_t>(i)];
                }
                if (OpenClipboard(hwnd)) {
                    EmptyClipboard();
                    const SIZE_T bytes = static_cast<SIZE_T>(text.size() + 1);
                    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                    if (mem) {
                        void *dst = GlobalLock(mem);
                        if (dst) {
                            std::memcpy(dst, text.c_str(), static_cast<std::size_t>(bytes));
                            GlobalUnlock(mem);
                            if (!SetClipboardData(CF_TEXT, mem)) GlobalFree(mem);
                        } else {
                            GlobalFree(mem);
                        }
                    }
                    CloseClipboard();
                }
                return 0;
            }
        }
        if (wParam == VK_ESCAPE) {
            self->selectAnchor_ = self->selectEnd_ = -1;
            self->selecting_ = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        RECT btns[kBtnCount];
        RECT rc{};
        GetClientRect(hwnd, &rc);
        layoutButtons(rc, btns);
        const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        for (int i = 0; i < kBtnCount; ++i) {
            if (PtInRect(&btns[i], pt)) {
                pressButton(hwnd, i);
                return 0;
            }
        }
        if (pt.y >= kHeaderH) {
            const int rows = std::max(1, static_cast<int>((rc.bottom - rc.top - kHeaderH) /
                                                          std::max(1, static_cast<int>(self->lineH_))));
            const int total = static_cast<int>(self->lines_.size());
            const int firstLine = std::max(0, total - rows + self->scrollOffset_);
            const int row = (pt.y - kHeaderH) / std::max(1, static_cast<int>(self->lineH_));
            const int idx = firstLine + row;
            if (idx >= 0 && idx < total) {
                self->selectAnchor_ = idx;
                self->selectEnd_ = idx;
                self->selecting_ = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (self->selecting_ && (wParam & MK_LBUTTON)) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int rows = std::max(1, static_cast<int>((rc.bottom - rc.top - kHeaderH) /
                                                          std::max(1, static_cast<int>(self->lineH_))));
            const int total = static_cast<int>(self->lines_.size());
            const int firstLine = std::max(0, total - rows + self->scrollOffset_);
            const int row = std::max(0, (GET_Y_LPARAM(lParam) - kHeaderH) /
                                           std::max(1, static_cast<int>(self->lineH_)));
            self->selectEnd_ = std::clamp(firstLine + row, 0, std::max(0, total - 1));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (self->selecting_) {
            self->selecting_ = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        break;
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
        if (delta != 0) self->scrollOffset_ = std::max(0, self->scrollOffset_ + delta);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE:
        self->setVisible(false);
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void DebugConsole::onPaint(HDC dc, const RECT &rcPaint) {
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

void DebugConsole::paintBuffer(HDC buffer, const RECT &full) {
    const Theme &t = theme();
    winutil::fillRectColor(buffer, full, t.panelBg);
    RECT rc{};
    GetClientRect(hwnd_, &rc);

    HFONT old = static_cast<HFONT>(SelectObject(buffer, font_));
    SetBkMode(buffer, TRANSPARENT);

    // ---- Header row: COM port/baud/mode + buttons (item 98) ----
    RECT headerRc{0, 0, full.right, kHeaderH};
    winutil::fillRectColor(buffer, headerRc, t.toolbarBg);

    char status[128];
    const SerialMonitor &mon = SerialMonitor::instance();
    if (mon.isOpen())
        std::snprintf(status, sizeof(status), "%s @%u %s", mon.portName().c_str(),
                      static_cast<unsigned>(mon.baud()), mon.hexMode() ? "HEX" : "ASCII");
    else
        std::snprintf(status, sizeof(status), "COM: --- (%s @%u)", Settings::get().comPort.c_str(),
                      static_cast<unsigned>(Settings::get().baud));
    winutil::drawTextOut(buffer, 6, 4, status, t.statusFg);

    RECT btns[kBtnCount];
    layoutButtons(rc, btns);
    for (int i = 0; i < kBtnCount; ++i) {
        winutil::fillRectColor(buffer, btns[i], mon.isOpen() || i != kBtnPort ? t.accentDim : t.panelBg);
        winutil::frameRectColor(buffer, btns[i], t.border);
        const char *label = buttonLabel(i);
        const int lw = static_cast<int>(strlen(label)) * charW_;
        winutil::drawTextOut(buffer, (btns[i].left + btns[i].right - lw) / 2,
                             (kHeaderH - lineH_) / 2 + 2,
                             label, mon.logging() && i == kBtnLog ? t.ok : t.fg);
    }
    RECT headerLine{0, kHeaderH, full.right, kHeaderH + 1};
    winutil::fillRectColor(buffer, headerLine, t.border);

    // ---- Lines below the header ----
    const int rows =
        std::max(1, static_cast<int>((rc.bottom - rc.top - kHeaderH) / std::max(1, static_cast<int>(lineH_))));
    const int total = static_cast<int>(lines_.size());
    const int firstLine = std::max(0, total - rows + scrollOffset_);
    for (int r = 0; r < rows; ++r) {
        const int idx = firstLine + r;
        if (idx >= total) break;
        const std::string &line = lines_[static_cast<std::size_t>(idx)];
        COLORREF color = t.fg;
        if (!line.empty() && line[0] == '[') {
            if (line.find("[ERR]") != std::string::npos || line.find("[Error]") != std::string::npos)
                color = t.err;
            else if (line.find("[WARN]") != std::string::npos)
                color = t.warn;
            else if (line.find("[RX ") == 0)
                color = t.ok;
            else if (line.find("[OK]") != std::string::npos)
                color = t.ok;
            else if (line.find("[INFO]") != std::string::npos)
                color = t.accent;
        }
        if (selectAnchor_ >= 0 && selectEnd_ >= 0) {
            const int lo = std::min(selectAnchor_, selectEnd_);
            const int hi = std::max(selectAnchor_, selectEnd_);
            if (idx >= lo && idx <= hi) {
                RECT selRc{0, kHeaderH + r * lineH_, full.right, kHeaderH + (r + 1) * lineH_};
                winutil::fillRectColor(buffer, selRc, t.selectionBg);
                color = t.selectionFg;
            }
        }
        winutil::drawTextOut(buffer, 6, kHeaderH + r * lineH_ + 1, line, color);
    }
    SelectObject(buffer, old);

    RECT border{full.right - 1, 0, full.right, full.bottom};
    winutil::fillRectColor(buffer, border, t.border);
}

