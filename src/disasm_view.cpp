#include "disasm_view.hpp"
#include "assembler.hpp"
#include "cpu.hpp"
#include "debugger.hpp"
#include "emulator.hpp"
#include "theme.hpp"
#include "win_util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) static_cast<int>(static_cast<short>(LOWORD(lp)))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) static_cast<int>(static_cast<short>(HIWORD(lp)))
#endif

bool DisasmView::create(HWND parent, HFONT font) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = &DisasmView::proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "6502DisasmView";
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    font_ = font;
    hwnd_ =
        CreateWindowExA(0, "6502DisasmView", nullptr, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS, 0,
                        0, 300, 200, parent, nullptr, GetModuleHandleA(nullptr), this);
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

void DisasmView::setFont(HFONT font) {
    font_ = font;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void DisasmView::attach(Emulator *emulator, Debugger *debugger) {
    emulator_ = emulator;
    debugger_ = debugger;
}

LRESULT CALLBACK DisasmView::proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTA *>(lParam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto *self = reinterpret_cast<DisasmView *>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
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
        self->updateScrollbar();
        self->hideTip();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_VSCROLL:
    case WM_MOUSEWHEEL: {
        self->hideTip();
        int delta = 0;
        if (msg == WM_MOUSEWHEEL)
            delta = -(GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA) * 3;
        else {
            const int action = LOWORD(wParam);
            if (action == SB_LINEUP)
                delta = -1;
            else if (action == SB_LINEDOWN)
                delta = 1;
            else if (action == SB_PAGEUP)
                delta = -self->rowsVisible(400);
            else if (action == SB_PAGEDOWN)
                delta = self->rowsVisible(400);
            else if (action == SB_THUMBTRACK || action == SB_THUMBPOSITION)
                self->topRow_ = HIWORD(wParam), InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        self->topRow_ += delta;
        self->topRow_ =
            std::clamp(self->topRow_, -8, static_cast<int>(self->rowAddrs_.size()) - self->rowsVisible(600));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        self->hideTip();
        SetFocus(hwnd);
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        const int row = self->topRow_ + std::max(0, y / self->lineH_);
        if (row >= 0 && row < static_cast<int>(self->rowAddrs_.size())) {
            const uint16_t addr = self->rowAddrs_[static_cast<std::size_t>(row)];
            if (x < self->charW_ * 4) {
                SendMessageA(GetParent(hwnd), WM_APP + 42, 1, static_cast<LPARAM>(addr));
            } else {
                self->selected_ = addr;
                SendMessageA(GetParent(hwnd), WM_APP + 42, 2, static_cast<LPARAM>(addr));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        // Hover tooltip only when no buttons are held.
        if ((wParam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) != 0) {
            self->hideTip();
            break;
        }
        if (!self->emulator_ || !self->emulator_->hasProgram()) {
            self->hideTip();
            break;
        }
        const int y = GET_Y_LPARAM(lParam);
        const int row = self->topRow_ + y / self->lineH_;
        if (row < 0 || row >= static_cast<int>(self->rowAddrs_.size())) {
            self->hideTip();
            break;
        }
        const uint16_t addr = self->rowAddrs_[static_cast<std::size_t>(row)];
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        self->tipTracked_ = true;
        self->showTip(addr, GET_X_LPARAM(lParam), y);
        return 0;
    }
    case WM_MOUSELEAVE:
        self->tipTracked_ = false;
        self->hideTip();
        break;
    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int DisasmView::rowsVisible(int height) const noexcept {
    return std::max(1, height / std::max(1, lineH_));
}

void DisasmView::updateScrollbar() {
    RECT rc{};
    if (!GetClientRect(hwnd_, &rc)) return;
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = static_cast<int>(rowAddrs_.size());
    si.nPage = static_cast<UINT>(std::max(1L, rc.bottom / lineH_));
    si.nPos = std::clamp(topRow_, 0, static_cast<int>(rowAddrs_.size()));
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void DisasmView::rebuildCacheFrom(uint16_t start) {
    if (!emulator_) return;
    const Memory &mem = emulator_->memory();
    rowAddrs_.clear();
    uint16_t addr = start;
    for (int i = 0; i < 512; ++i) {
        rowAddrs_.push_back(addr);
        addr = static_cast<uint16_t>(addr + CPU::instructionLength(mem.read(addr)));
    }
    tipCache_.clear();
    updateScrollbar();
}

void DisasmView::centerOn(uint16_t address) {
    rebuildCacheFrom(static_cast<uint16_t>(address & ~1u));
    selected_ = address;
    for (std::size_t i = 0; i < rowAddrs_.size(); ++i) {
        if (rowAddrs_[i] == address ||
            (i + 1 < rowAddrs_.size() && rowAddrs_[i] < address && rowAddrs_[i + 1] > address)) {
            topRow_ = static_cast<int>(i) - rowsVisible(500) / 3;
            break;
        }
    }
    topRow_ = std::clamp(topRow_, -8, static_cast<int>(rowAddrs_.size()) - rowsVisible(600));
    updateScrollbar();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void DisasmView::jumpToPc() {
    if (!emulator_) return;
    centerOn(emulator_->cpu().PC);
}

std::string DisasmView::tipTextFor(uint16_t addr) {
    const auto cached = tipCache_.find(addr);
    if (cached != tipCache_.end()) return cached->second;

    const CPU &cpu = emulator_->cpu();
    const Memory &mem = emulator_->memory();
    const uint8_t op = mem.read(addr);
    uint8_t len = 0;
    int cycles = 0;
    bool isPc = false;
    cpu.disassembleOneAt(addr, len, cycles, isPc);

    const Assembler::OpcodeInfo *info = Assembler::findOpcodeByByte(op);
    char buf[160];
    if (!info) {
        std::snprintf(buf, sizeof(buf), "%d cycles (undocumented opcode %02X)", cycles,
                      static_cast<unsigned>(op));
        tipCache_.emplace(addr, buf);
        return buf;
    }

    int base = info->cycles;
    std::string extra;

    const bool isBranch = (op & 0x1Fu) == 0x10u; // x10 relative-branch encodings
    if (isBranch) {
        const int offset = static_cast<int8_t>(mem.read(static_cast<uint16_t>(addr + 1)));
        const uint16_t next = static_cast<uint16_t>(addr + 2);
        const uint16_t target = static_cast<uint16_t>(next + offset);
        const bool cross = ((next ^ target) & 0xFF00u) != 0;
        extra = "+1 taken branch";
        if (cross) extra += ", +1 page cross";
        base = cycles - (cross ? 2 : 1); // disassembleOneAt already added the penalty
    } else if (info->pagePenalty && len == 3) {
        const uint16_t baseAddr =
            static_cast<uint16_t>(mem.read(static_cast<uint16_t>(addr + 1)) |
                                  (static_cast<uint16_t>(mem.read(static_cast<uint16_t>(addr + 2))) << 8));
        const int idx = (info->mode == Assembler::AddrMode::AbsoluteX)
                            ? cpu.X
                            : (info->mode == Assembler::AddrMode::AbsoluteY ? cpu.Y : 0);
        const bool cross = ((baseAddr ^ static_cast<uint16_t>(baseAddr + idx)) & 0xFF00u) != 0;
        if (cross && cycles > base) extra = "+1 page cross";
        else if (!cross) extra = "no page cross";
    }

    // Read-modify-write opcodes perform a double write on the bus.
    static const char *const kRmw[] = {"ASL", "LSR", "ROL", "ROR", "INC", "DEC",
                                       "SLO", "RLA", "SRE", "RRA", "DCP", "ISC"};
    bool rmw = false;
    for (const char *n : kRmw)
        if (std::strcmp(info->name, n) == 0) {
            rmw = info->mode != Assembler::AddrMode::Accumulator &&
                  info->mode != Assembler::AddrMode::Implied;
            break;
        }
    if (!extra.empty() && rmw) extra += ", RMW";
    else if (rmw) extra = "RMW (read-modify-write)";

    if (extra.empty())
        std::snprintf(buf, sizeof(buf), "%d cycles (base %d)", cycles, base);
    else
        std::snprintf(buf, sizeof(buf), "%d cycles = base %d %s", cycles, base, extra.c_str());
    tipCache_.emplace(addr, buf);
    return buf;
}

LRESULT CALLBACK DisasmView::tipProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        auto *self = reinterpret_cast<DisasmView *>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const Theme &t = theme();
        winutil::fillRectColor(dc, rc, blend(t.panelBg, RGB(0, 0, 0), 0.35));
        winutil::frameRectColor(dc, rc, t.accentDim);
        if (self && !self->tipText_.empty()) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, t.fg);
            TextOutA(dc, 6, 3, self->tipText_.c_str(), static_cast<int>(self->tipText_.size()));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

void DisasmView::showTip(uint16_t addr, int clientX, int clientY) {
    const std::string text = tipTextFor(addr);
    if (text.empty()) {
        hideTip();
        return;
    }
    tipText_ = text;
    if (!tip_) {
        WNDCLASSA wc{};
        wc.lpfnWndProc = &DisasmView::tipProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "6502DisasmTip";
        RegisterClassA(&wc);
        tip_ = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, "6502DisasmTip",
                               nullptr, WS_POPUP, 0, 0, 10, 10, hwnd_, nullptr,
                               GetModuleHandleA(nullptr), this);
        if (!tip_) return;
        SetWindowLongPtrA(tip_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }

    // Size the popup to the text using the same font as the view.
    HDC dc = GetDC(tip_);
    HFONT old = static_cast<HFONT>(SelectObject(dc, font_));
    SIZE sz{};
    GetTextExtentPoint32A(dc, text.c_str(), static_cast<int>(text.size()), &sz);
    SelectObject(dc, old);
    ReleaseDC(tip_, dc);
    const int w = sz.cx + 12;
    const int h = sz.cy + 6;

    POINT pt{clientX + 14, clientY + lineH_ + 4};
    MapWindowPoints(hwnd_, nullptr, &pt, 1);
    RECT wa{};
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    pt.x = std::min(std::max(wa.left, pt.x), std::max(wa.left, wa.right - w - 2));
    pt.y = std::min(std::max(wa.top, pt.y), std::max(wa.top, wa.bottom - h - 2));

    SetWindowPos(tip_, HWND_TOPMOST, pt.x, pt.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
}

void DisasmView::hideTip() {
    tipTracked_ = false;
    if (tip_) ShowWindow(tip_, SW_HIDE);
}

void DisasmView::refresh(bool force) {
    if (!emulator_) return;
    const uint16_t pc = emulator_->cpu().PC;
    bool pcVisible = false;
    for (uint16_t a : rowAddrs_)
        if (a == pc) {
            pcVisible = true;
            break;
        }
    if (rowAddrs_.empty() || !pcVisible || force) {
        if (rowAddrs_.empty()) {
            rebuildCacheFrom(pc);
        } else {
            jumpToPc();
        }
    }
    if (force || pc != lastPc_) {
        lastPc_ = pc;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void DisasmView::paintBuffer(HDC buffer, const RECT &full) {
    const Theme &t = theme();
    winutil::fillRectColor(buffer, full, t.panelBg);
    if (!emulator_) return;

    // Show nothing until a program is burned — keeps UI clean at startup.
    if (!emulator_->hasProgram()) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        HFONT oldFont = static_cast<HFONT>(SelectObject(buffer, font_));
        SetBkMode(buffer, TRANSPARENT);
        const char *hint = "No program loaded - press Build / Burn+Run";
        SetTextColor(buffer, t.dimFg);
        TextOutA(buffer, charW_ * 2, (rc.bottom - rc.top) / 2 - lineH_ / 2, hint,
                 static_cast<int>(strlen(hint)));
        SelectObject(buffer, oldFont);
        return;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int rows = rowsVisible(static_cast<int>(rc.bottom - rc.top)) + 1;
    HFONT oldFont = static_cast<HFONT>(SelectObject(buffer, font_));
    SetBkMode(buffer, TRANSPARENT);

    const CPU &cpu = emulator_->cpu();
    const auto &symbols = emulator_->program().symbols;

    for (int r = 0; r <= rows; ++r) {
        const int row = topRow_ + r;
        if (row < 0 || row >= static_cast<int>(rowAddrs_.size())) continue;
        const uint16_t addr = rowAddrs_[static_cast<std::size_t>(row)];
        const int y = r * lineH_;

        const bool isPc = addr == cpu.PC;
        const bool isSel = addr == selected_;

        if (isPc) {
            RECT bg{0, y, full.right, y + lineH_};
            winutil::fillRectColor(buffer, bg, blend(t.panelBg, t.disasmCurrent, 0.16));
        } else if (isSel) {
            RECT bg{0, y, full.right, y + lineH_};
            winutil::fillRectColor(buffer, bg, t.curLine);
        }

        bool bpHere = false;
        if (debugger_) {
            for (const auto &bp : debugger_->breakpoints())
                if (bp.address == addr && bp.enabled) {
                    bpHere = true;
                    break;
                }
        }

        if (bpHere) {
            HBRUSH red = CreateSolidBrush(t.disasmBp);
            HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(buffer, red));
            Ellipse(buffer, charW_ / 2, y + lineH_ / 2 - 4, charW_ / 2 + 9, y + lineH_ / 2 + 5);
            SelectObject(buffer, oldBr);
            DeleteObject(red);

            // Item 53: inline hit counter, right-aligned in the gutter.
            if (debugger_) {
                const int hits = debugger_->breakpointHitCount(addr);
                if (hits > 0) {
                    char cnt[16];
                    std::snprintf(cnt, sizeof(cnt), "%d", hits);
                    const int digits = static_cast<int>(strlen(cnt));
                    const int cx = std::max(charW_ / 2 + 11, charW_ * 4 - digits * charW_ - 1);
                    SetTextColor(buffer, t.accent);
                    TextOutA(buffer, cx, y, cnt, digits);
                }
            }
        }

        if (isPc) {
            SetTextColor(buffer, t.disasmCurrent);
            TextOutA(buffer, charW_ * 2, y, ">", 1);
        }

        char buf[64];
        std::snprintf(buf, sizeof(buf), "%04X", addr);
        SetTextColor(buffer, isSel ? t.selectionFg : t.hexAddr);
        TextOutA(buffer, charW_ * 4, y, buf, 4);

        const Memory &mem = emulator_->memory();
        const int len = CPU::instructionLength(mem.read(addr));
        std::string rawBytes;
        char one[4];
        for (int i = 0; i < len; ++i) {
            std::snprintf(one, sizeof(one), "%02X ", mem.read(static_cast<uint16_t>(addr + i)));
            rawBytes += one;
        }
        SetTextColor(buffer, t.dimFg);
        TextOutA(buffer, charW_ * 9, y, rawBytes.c_str(), static_cast<int>(rawBytes.size()));

        uint8_t lenOut = 0;
        int cycles = 0;
        bool pcFlag = false;
        const std::string text = cpu.disassembleOneAt(addr, lenOut, cycles, pcFlag);

        std::string mnemonic = text.substr(2);
        const auto spacePos = mnemonic.find(' ');
        std::string opPart = mnemonic.substr(spacePos + 1);

        auto resolveLabel = [&](const std::map<std::string, uint16_t> &syms,
                                uint16_t value) -> const std::string * {
            for (const auto &kv : syms)
                if (kv.second == value && kv.first.find('@') == std::string::npos && kv.first != "START" &&
                    kv.first != "RESET")
                    return &kv.first;
            return nullptr;
        };

        if ((opPart.rfind("$", 0) == 0) && !symbols.empty()) {
            const unsigned target = std::strtoul(opPart.c_str() + 1, nullptr, 16);
            if (const std::string *label = resolveLabel(symbols, static_cast<uint16_t>(target))) {
                opPart = *label;
            }
        }

        std::snprintf(buf, sizeof(buf), "%-4s %-14s", text.substr(2, 3).c_str(), opPart.c_str());
        SetTextColor(buffer, isPc ? t.fg : (isSel ? t.selectionFg : t.fg));
        TextOutA(buffer, charW_ * 17, y, buf, static_cast<int>(strlen(buf)));

        std::snprintf(buf, sizeof(buf), "%d", cycles);
        SetTextColor(buffer, t.dimFg);
        TextOutA(buffer, full.right - 6 * charW_, y, buf, static_cast<int>(strlen(buf)));
    }

    SelectObject(buffer, oldFont);
}

void DisasmView::onPaint(HDC dc, const RECT &rcPaint) {
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
