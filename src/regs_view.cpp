#include "regs_view.hpp"
#include "cpu.hpp"
#include "debugger.hpp"
#include "emulator.hpp"
#include "theme.hpp"
#include "win_util.hpp"

#include <cstdio>
#include <cstring>

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) static_cast<LONG>(static_cast<short>(LOWORD(lp)))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) static_cast<LONG>(static_cast<short>(HIWORD(lp)))
#endif

bool RegsView::create(HWND parent, HFONT font, HFONT boldFont) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = &RegsView::proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "6502RegsView";
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    font_ = font;
    bold_ = boldFont;
    hwnd_ = CreateWindowExA(0, "6502RegsView", nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 240,
                            200, parent, nullptr, GetModuleHandleA(nullptr), this);
    return hwnd_ != nullptr;
}

void RegsView::setFont(HFONT font, HFONT boldFont) {
    font_ = font;
    bold_ = boldFont;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void RegsView::refresh() {
    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK RegsView::proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTA *>(lParam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto *self = reinterpret_cast<RegsView *>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcA(hwnd, msg, wParam, lParam);

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
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        if (!self->emulator_) return 0;
        if (self->emulator_->running()) {
            SendMessageA(GetParent(hwnd), WM_APP + 43, 0, 0);
            return 0;
        }
        const LONG y = GET_Y_LPARAM(lParam);
        const LONG x = GET_X_LPARAM(lParam);
        static const char *const flagNames[8] = {"N", "V", "-", "B", "D", "I", "Z", "C"};
        if (y >= 58 && y <= 76) {
            for (int pos = 0; pos < 8; ++pos) {
                const int cx = 96 + pos * 20;
                if (x >= cx && x <= cx + 16) {
                    if (!std::strcmp(flagNames[pos], "-") || !std::strcmp(flagNames[pos], "B")) break;
                    uint8_t flag = 0;
                    switch (flagNames[pos][0]) {
                    case 'N':
                        flag = FLAG_N;
                        break;
                    case 'V':
                        flag = FLAG_V;
                        break;
                    case 'D':
                        flag = FLAG_D;
                        break;
                    case 'I':
                        flag = FLAG_I;
                        break;
                    case 'Z':
                        flag = FLAG_Z;
                        break;
                    case 'C':
                        flag = FLAG_C;
                        break;
                    }
                    self->emulator_->cpu().P ^= flag;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
            }
        }
        return 0;
    }
    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

void RegsView::paintBuffer(HDC buffer, const RECT &full) {
    const Theme &t = theme();
    winutil::fillRectColor(buffer, full, t.panelBg);
    RECT rc{};
    GetClientRect(hwnd_, &rc);

    SelectObject(buffer, bold_);
    SetBkMode(buffer, TRANSPARENT);
    winutil::drawTextOut(buffer, 10, 8, "CPU STATE", t.accent);
    SelectObject(buffer, font_);

    if (!emulator_) {
        SelectObject(buffer, bold_);
        return;
    }

    const CPU &c = emulator_->cpu();
    char buf[128];
    struct Row {
        const char *name;
        unsigned value;
        int digits;
    };
    const Row rows[] = {
        {"A", c.A, 2}, {"X", c.X, 2}, {"Y", c.Y, 2}, {"PC", c.PC, 4}, {"SP", c.SP, 2}, {"P", c.P, 2},
    };
    int y = 30;
    for (const Row &row : rows) {
        std::snprintf(buf, sizeof(buf), "%-3s", row.name);
        winutil::drawTextOut(buffer, 14, y, buf, t.dimFg);
        std::snprintf(buf, sizeof(buf), "$%0*X", row.digits, row.value);
        winutil::drawTextOut(buffer, 44, y, buf, t.fg);
        y += lineSpacing();
    }

    SelectObject(buffer, font_);
    static const char *const chipNames[8] = {"N", "V", "-", "B", "D", "I", "Z", "C"};
    const bool paused = !emulator_->running();
    for (int pos = 0; pos < 8; ++pos) {
        const char *nameCh = chipNames[pos];
        const int bit = 7 - pos;
        const bool set = (c.P >> bit) & 1u;
        const int cx = 96 + pos * 20;
        RECT chipRc{cx, 58, cx + 16, 76};
        COLORREF fill = set ? t.accent : blend(t.panelBg, t.fg, 0.12);
        winutil::fillRectColor(buffer, chipRc, fill);
        // Contrast-safe label: pick dark or light text based on fill luminance.
        const int lum = (static_cast<int>(fill & 0xFF) * 299 + static_cast<int>((fill >> 8) & 0xFF) * 587 +
                         static_cast<int>((fill >> 16) & 0xFF) * 114) /
                        1000;
        SetTextColor(buffer, set ? (lum >= 128 ? RGB(24, 26, 34) : RGB(244, 245, 250)) : t.dimFg);
        TextOutA(buffer, cx + 5, 61, nameCh, 1);
        if (paused && std::strcmp(nameCh, "-") != 0 && std::strcmp(nameCh, "B") != 0) {
            winutil::frameRectColor(buffer, chipRc, t.border);
        }
    }

    SelectObject(buffer, font_);
    y += 8;

    std::snprintf(buf, sizeof(buf), "OP %02X %-4s %dc", c.lastOpcode(), CPU::opcodeName(c.lastOpcode()),
                  c.getCycles());
    winutil::drawTextOut(buffer, 14, y, buf, t.fg);
    y += lineSpacing();
    std::snprintf(buf, sizeof(buf), "Cycles %llu", static_cast<unsigned long long>(c.getTotalCycles()));
    winutil::drawTextOut(buffer, 14, y, buf, t.dimFg);
    y += lineSpacing();

    const uint16_t nmiVec =
        static_cast<uint16_t>(emulator_->memory().read(0xFFFA) | (emulator_->memory().read(0xFFFB) << 8));
    const uint16_t rstVec =
        static_cast<uint16_t>(emulator_->memory().read(0xFFFC) | (emulator_->memory().read(0xFFFD) << 8));
    const uint16_t irqVec =
        static_cast<uint16_t>(emulator_->memory().read(0xFFFE) | (emulator_->memory().read(0xFFFF) << 8));
    std::snprintf(buf, sizeof(buf), "NMI %04X RST %04X IRQ %04X", nmiVec, rstVec, irqVec);
    winutil::drawTextOut(buffer, 14, y, buf, t.hexAddr);
    y += lineSpacing();

    if (debugger_) {
        const auto &watches = debugger_->watchExpressions();
        int shown = 0;
        for (std::size_t i = 0; i < watches.size() && shown < 3; ++i, ++shown) {
            uint64_t value = 0;
            std::string err;
            char watch[160];
            if (debugger_->evaluateExpression(watches[i], value, err))
                std::snprintf(watch, sizeof(watch), "w %s=$%llX", watches[i].substr(0, 12).c_str(),
                              static_cast<unsigned long long>(value));
            else
                std::snprintf(watch, sizeof(watch), "w %s=?", watches[i].substr(0, 14).c_str());
            winutil::drawTextOut(buffer, 14, y, watch, t.tokNumber);
            y += lineSpacing();
        }
    }

    const COLORREF stateColor = c.halted() ? t.err : (emulator_->running() ? t.ok : t.warn);
    std::snprintf(buf, sizeof(buf), "%s",
                  c.halted() ? "HALTED" : (emulator_->running() ? "RUNNING" : "PAUSED"));
    SelectObject(buffer, bold_);
    winutil::drawTextOut(buffer, 14, y, buf, stateColor);
    SelectObject(buffer, font_);

    RECT border{full.right - 1, 0, full.right, full.bottom};
    winutil::fillRectColor(buffer, border, t.border);
}

void RegsView::onPaint(HDC dc, const RECT &rcPaint) {
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
