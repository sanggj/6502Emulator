#include "main_ui.hpp"
#include "app.hpp"
#include "settings.hpp"
#include "theme.hpp"
#include "win_util.hpp"

#ifdef _WIN32
#include <commctrl.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) static_cast<int>(static_cast<short>(LOWORD(lp)))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) static_cast<int>(static_cast<short>(HIWORD(lp)))
#endif

namespace {

struct ToolButton {
    const char *label;
    CommandId id;
    int width;
};

const ToolButton kButtons[] = {
    {"Build", ID_BUILD_BUILD, 60}, {"Burn+Run", ID_BUILD_BURNRUN, 74}, {"Upload", ID_BUILD_UPLOAD, 62},
    {"Run", ID_DEBUG_RUN, 48},     {"Pause", ID_DEBUG_PAUSE, 54},      {"Step", ID_DEBUG_STEP, 50},
    {"Reset", ID_DEBUG_RESET, 54}, {"Back", ID_DEBUG_BACK, 50},        {"Fwd", ID_DEBUG_FORWARD, 46},
    {"BP", ID_DEBUG_TOGGLEBP, 42}, {"Shot", ID_VIEW_SCREENSHOT, 52},
};

int g_hoverButton = -1;
int g_pressedButton = -1;
bool g_dragging = false;

std::string g_statusMessage = "Ready";
std::string g_ipsText = "0 ips";

// Live performance counters (item 33).
double g_perfMips = 0.0;
int g_perfFps = 0;
uint64_t g_perfCycles = 0;

// OSD toast state (item 34): one message at a time, auto-expires.
std::string g_toastText;
std::chrono::steady_clock::time_point g_toastExpiry{};
bool g_toastActive = false;

RECT g_rcEditor{}, g_rcScreen{}, g_rcTerm{}, g_rcInput{}, g_rcRegs{}, g_rcHex{}, g_rcDisasm{};
RECT g_rcToolbar{}, g_rcStatus{}, g_rcSplitV1{}, g_rcSplitV2{}, g_rcSplitMidH{};
// Splitters inside the right column: regs|hex and hex|disasm boundaries.
RECT g_rcSplitRegHex{}, g_rcSplitHexDis{};
int g_dragMode = 0;
int g_dragStartX = 0;
int g_dragStartY = 0;
int g_dragOrigPct = 0;

constexpr int kPad = 6;
constexpr int kToolH = 38;
constexpr int kStatusH = 26;
constexpr int kMinLeftW = 240;
constexpr int kMinMidW = 200;
constexpr int kMinRightW = 220;

std::string fileLeaf(const std::string &path) {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string toLowerCopy(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string thousands(uint64_t value) {
    char raw[24];
    std::snprintf(raw, sizeof(raw), "%llu", static_cast<unsigned long long>(value));
    std::string out;
    const std::size_t n = std::strlen(raw);
    for (std::size_t i = 0; i < n; ++i) {
        out += raw[i];
        if (const std::size_t rest = n - i - 1; rest > 0 && rest % 3 == 0 && i + 1 < n) out += ',';
    }
    return out;
}

// Relative luminance (Rec. 601) of a COLORREF, 0..255.
int luminance(COLORREF c) {
    return (static_cast<int>(c & 0xFF) * 299 + static_cast<int>((c >> 8) & 0xFF) * 587 +
            static_cast<int>((c >> 16) & 0xFF) * 114) /
           1000;
}

// Pick near-white or near-black text so labels stay readable on ANY fill,
// including hover/pressed states in every theme.
COLORREF contrastTextFor(COLORREF fill) {
    return luminance(fill) >= 128 ? RGB(24, 26, 34) : RGB(244, 245, 250);
}

} // namespace

void statusBarSetText(const std::string &text) {
    g_statusMessage = text;
}

void setIpsText(const std::string &text) {
    g_ipsText = text;
}

void uiSetPerf(double mips, int fps, uint64_t totalCycles) {
    g_perfMips = mips;
    g_perfFps = fps;
    g_perfCycles = totalCycles;
}

// ---- OSD toast notifications (item 34) ----

namespace {

RECT toastInvalidateRect() {
    RECT rc{};
    if (g_app.mainWnd) GetClientRect(g_app.mainWnd, &rc);
    RECT r{std::max<LONG>(0, rc.right - 380), std::max<LONG>(0, rc.bottom - 90), rc.right, rc.bottom};
    return r;
}

} // namespace

void osdToast(const std::string &text) {
    g_toastText = text.substr(0, 120);
    g_toastExpiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(1800);
    g_toastActive = true;
    if (g_app.mainWnd) {
        const RECT r = toastInvalidateRect();
        InvalidateRect(g_app.mainWnd, &r, FALSE);
    }
}

void uiTickToasts() {
    if (!g_toastActive || !g_app.mainWnd) return;
    const bool expired = std::chrono::steady_clock::now() >= g_toastExpiry;
    if (expired) g_toastActive = false; // repaint once more to erase
    const RECT r = toastInvalidateRect();
    InvalidateRect(g_app.mainWnd, &r, FALSE);
}

void uiPaintToasts(HDC dc, const RECT &client) {
    if (!g_toastActive || g_toastText.empty()) return;
    const Theme &t = theme();
    SelectObject(dc, g_app.uiFont);
    SetBkMode(dc, TRANSPARENT);
    SIZE sz{};
    GetTextExtentPoint32A(dc, g_toastText.c_str(), static_cast<int>(g_toastText.size()), &sz);
    const int w = sz.cx + 28;
    const int h = sz.cy + 16;
    RECT r{client.right - w - 16, client.bottom - h - kStatusH - 14, client.right - 16,
           client.bottom - kStatusH - 14};
    HBRUSH brush = CreateSolidBrush(t.panelBg);
    HPEN pen = CreatePen(PS_SOLID, 1, t.accent);
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, brush));
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, pen));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 12, 12);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
    winutil::drawTextOut(dc, r.left + 14, r.top + (r.bottom - r.top - sz.cy) / 2, g_toastText, t.fg);
}

RECT uiToolbarRect() {
    return g_rcToolbar;
}
RECT uiStatusRect() {
    return g_rcStatus;
}
RECT uiSplitterRect(int which) {
    switch (which) {
    case 1:
        return g_rcSplitV1;
    case 2:
        return g_rcSplitV2;
    case 3:
        return g_rcSplitMidH;
    case 4:
        return g_rcSplitRegHex;
    default:
        return g_rcSplitHexDis;
    }
}

void uiPaintToolbar(HDC dc, const RECT &rc) {
    const Theme &t = theme();
    winutil::fillRectColor(dc, rc, t.toolbarBg);
    RECT border{rc.left, rc.bottom - 1, rc.right, rc.bottom};
    winutil::fillRectColor(dc, border, t.border);
    SelectObject(dc, g_app.uiFont);
    SetBkMode(dc, TRANSPARENT);
    int x = 8;
    const int bh = 26;
    const int by = (rc.bottom - rc.top - bh) / 2;
    for (int i = 0; i < static_cast<int>(std::size(kButtons)); ++i) {
        const int bw = kButtons[i].width;
        RECT btnRc{x, by, x + bw, by + bh};
        // Soft, low-glare button states. Fill and text are always paired for
        // contrast, so labels never fade into the background on hover.
        COLORREF fill = blend(t.toolbarBg, t.fg, 0.05); // gentle raised surface
        COLORREF edge = blend(t.border, t.toolbarBg, 0.35);
        COLORREF text = t.fg;
        if (i == g_pressedButton) {
            fill = t.accentDim;
            edge = t.accent;
            text = contrastTextFor(fill);
        } else if (i == g_hoverButton) {
            fill = blend(t.toolbarBg, t.accent, 0.30); // calm accent tint
            edge = t.accent;
            text = contrastTextFor(fill);
        }
        HBRUSH brush = CreateSolidBrush(fill);
        HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, brush));
        HPEN pen = CreatePen(PS_SOLID, 1, edge);
        HPEN oldPen = static_cast<HPEN>(SelectObject(dc, pen));
        Rectangle(dc, btnRc.left, btnRc.top, btnRc.right, btnRc.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
        SIZE sz{};
        GetTextExtentPoint32A(dc, kButtons[i].label, static_cast<int>(strlen(kButtons[i].label)), &sz);
        winutil::drawTextOut(dc, btnRc.left + (bw - sz.cx) / 2, by + (bh - sz.cy) / 2, kButtons[i].label,
                             text);
        x += bw + 6;
    }
}

int uiToolbarButtonAt(int x, int y) {
    if (y < g_rcToolbar.top || y >= g_rcToolbar.bottom - 1) return -1;
    int cx = 8;
    for (int i = 0; i < static_cast<int>(std::size(kButtons)); ++i) {
        if (x >= cx && x < cx + kButtons[i].width) return i;
        cx += kButtons[i].width + 6;
    }
    return -1;
}

void uiSetPressedButton(int index) {
    g_pressedButton = index;
}
int uiPressedButton() noexcept {
    return g_pressedButton;
}
int uiHoverButton() noexcept {
    return g_hoverButton;
}
void uiSetHoverButton(int index) {
    g_hoverButton = index;
}
int uiButtonCommandId(int index) {
    return (index >= 0 && index < static_cast<int>(std::size(kButtons)))
               ? static_cast<int>(kButtons[index].id)
               : 0;
}

void uiPaintStatus(HDC dc, const RECT &rc) {
    const Theme &t = theme();
    winutil::fillRectColor(dc, rc, t.statusBg);
    SelectObject(dc, g_app.uiFont);
    SetBkMode(dc, TRANSPARENT);

    const CPU &cpu = g_app.emu.cpu();
    char seg[192];
    // Right-anchored segments: L:C, file, FPS/MIPS/cycles, ips/state, regs.
    const int xPos = rc.right - 64;
    const int xFile = rc.right - 210;
    const int xPerf = rc.right - 500;
    const int xRun = rc.right - 660;
    const int xRegs = rc.right - 920;

    winutil::drawTextOut(dc, rc.left + 8, rc.top + 5, g_statusMessage.substr(0, 80), t.statusFg);

    if (rc.right >= 950) {
        std::snprintf(seg, sizeof(seg), "PC %04X  A %02X X %02X Y %02X", cpu.PC, cpu.A, cpu.X, cpu.Y);
        winutil::drawTextOut(dc, xRegs, rc.top + 5, seg, t.fg);
    }
    std::snprintf(seg, sizeof(seg), "%s | %s", g_ipsText.c_str(),
                  cpu.halted() ? "HALT" : (g_app.emu.running() ? "RUN" : "PAUSE"));
    winutil::drawTextOut(dc, xRun, rc.top + 5, seg,
                         cpu.halted() ? t.err : (g_app.emu.running() ? t.ok : t.warn));

    std::snprintf(seg, sizeof(seg), "FPS %d | %.2f MIPS | %s cyc", g_perfFps, g_perfMips,
                  thousands(g_perfCycles).c_str());
    winutil::drawTextOut(dc, xPerf, rc.top + 5, seg, t.dimFg);

    std::string file = g_app.currentFile.empty() ? "(untitled)" : fileLeaf(g_app.currentFile);
    std::snprintf(seg, sizeof(seg), "%s %s", file.c_str(), g_app.editor.modified() ? "*" : "");
    winutil::drawTextOut(dc, xFile, rc.top + 5, seg, t.dimFg);

    std::snprintf(seg, sizeof(seg), "L%d:%d", g_app.editor.caretLine() + 1, g_app.editor.caretColumn() + 1);
    winutil::drawTextOut(dc, xPos, rc.top + 5, seg, t.dimFg);

    RECT border{rc.left, rc.top, rc.right, rc.top + 1};
    winutil::fillRectColor(dc, border, t.border);
}

void uiComputeLayout(int w, int h) {
    AppSettings &s = Settings::get();
    g_rcToolbar = {0, 0, w, kToolH};
    g_rcStatus = {0, h - kStatusH, w, h};
    const int contentTop = kToolH + kPad;
    const int contentBottom = h - kStatusH - kPad;
    const int contentH = std::max(120, contentBottom - contentTop);

    // Three columns, each width user-resizable (VS Code style).
    const int maxRightW = std::max(kMinRightW, w * 45 / 100);
    const int rightW = std::clamp(w * s.rightPct / 100, kMinRightW, maxRightW);
    const int maxLeftW = std::max(kMinLeftW, w - rightW - kMinMidW - kPad * 3);
    const int leftW = std::clamp(w * s.maxLeftPct / 100, kMinLeftW, maxLeftW);
    const int midW = std::max(kMinMidW, w - leftW - rightW - kPad * 3);
    const int x1 = leftW + kPad;
    const int x2 = x1 + midW + kPad;

    g_rcEditor = {kPad, contentTop, leftW, contentBottom};
    g_rcSplitV1 = {leftW, contentTop, leftW + kPad, contentBottom};

    const int screenH = std::clamp(contentH * s.midSplitPct / 100, 60, contentH - 90);
    g_rcScreen = {x1, contentTop, x1 + midW, contentTop + screenH};
    g_rcSplitMidH = {x1, g_rcScreen.bottom, x1 + midW, g_rcScreen.bottom + kPad};
    g_rcTerm = {x1, g_rcSplitMidH.bottom, x1 + midW, contentBottom - 30};
    g_rcInput = {x1, contentBottom - 27, x1 + midW, contentBottom};

    g_rcSplitV2 = {x2 - kPad, contentTop, x2, contentBottom};

    // Right column: regs / hex / disasm stacked with two draggable splitters.
    const int colL = x2;
    const int colR = std::min(x2 + rightW, w);
    int regsH = std::clamp(contentH * s.regsPct / 100, 56, contentH - 96);
    regsH = std::max(56, regsH); // layout clamp above already leaves >=96px below
    const int remAfterRegs = std::max(84, contentH - regsH - kPad);
    const int hexH = std::clamp(remAfterRegs * s.hexPct / 100, 40, remAfterRegs - 44);
    g_rcRegs = {colL, contentTop, colR, contentTop + regsH};
    g_rcSplitRegHex = {colL, contentTop + regsH, colR, contentTop + regsH + kPad};
    const int hexTop = contentTop + regsH + kPad;
    g_rcHex = {colL, hexTop, colR, hexTop + hexH};
    g_rcSplitHexDis = {colL, hexTop + hexH, colR, hexTop + hexH + kPad};
    const int disasmTop = hexTop + hexH + kPad;
    g_rcDisasm = {colL, disasmTop, colR, std::max(disasmTop + 40, contentBottom)};
}

void uiApplyLayout() {
    RECT rc{};
    GetClientRect(g_app.mainWnd, &rc);
    uiComputeLayout(rc.right, rc.bottom);
    auto move = [&](HWND hwnd, const RECT &r) {
        MoveWindow(hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
    };
    move(g_app.editor.hwnd(), g_rcEditor);
    move(g_app.disp.hwnd(), g_rcScreen);
    move(g_app.term.hwnd(), g_rcTerm);
    move(g_app.cmdInput, g_rcInput);
    move(g_app.regs.hwnd(), g_rcRegs);
    move(g_app.hex.hwnd(), g_rcHex);
    move(g_app.disasm.hwnd(), g_rcDisasm);
}

int uiSplitterHitTest(int x, int y) {
    auto inRect = [](const RECT &r, int px, int py) {
        return px >= r.left && px <= r.right && py >= r.top && py <= r.bottom;
    };
    if (inRect(g_rcSplitV1, x, y)) return 1;
    if (inRect(g_rcSplitV2, x, y)) return 2;
    if (inRect(g_rcSplitMidH, x, y)) return 3;
    if (inRect(g_rcSplitRegHex, x, y)) return 4;
    if (inRect(g_rcSplitHexDis, x, y)) return 5;
    return 0;
}

void uiBeginDrag(int mode, int startX, int startY) {
    g_dragMode = mode;
    g_dragging = true;
    g_dragStartX = startX;
    g_dragStartY = startY;
    AppSettings &s = Settings::get();
    switch (mode) {
    case 2: g_dragOrigPct = s.rightPct; break;
    case 3: g_dragOrigPct = s.midSplitPct; break;
    case 4: g_dragOrigPct = s.regsPct; break;
    case 5: g_dragOrigPct = s.hexPct; break;
    default: g_dragOrigPct = s.maxLeftPct; break;
    }
}

void uiDragTo(int x, int y) {
    if (!g_dragMode) return;
    RECT rc{};
    GetClientRect(g_app.mainWnd, &rc);
    const int w = std::max(1, static_cast<int>(rc.right));
    const int h = std::max(1, static_cast<int>(rc.bottom));
    AppSettings &s = Settings::get();
    switch (g_dragMode) {
    case 1:
        s.maxLeftPct =
            std::clamp(g_dragOrigPct + (x - g_dragStartX) * 100 / w, 15, 70);
        break;
    case 2:
        s.rightPct = std::clamp(g_dragOrigPct - (x - g_dragStartX) * 100 / w, 12, 45);
        break;
    case 3:
        s.midSplitPct = std::clamp(g_dragOrigPct + (y - g_dragStartY) * 100 / h, 20, 85);
        break;
    case 4:
        s.regsPct = std::clamp(g_dragOrigPct + (y - g_dragStartY) * 100 / h, 10, 80);
        break;
    case 5:
        s.hexPct = std::clamp(g_dragOrigPct + (y - g_dragStartY) * 100 / h, 10, 80);
        break;
    default:
        break;
    }
    uiApplyLayout();
    InvalidateRect(g_app.mainWnd, nullptr, FALSE);
}

bool uiDragging() {
    return g_dragging;
}

void uiEndDrag() {
    if (g_dragMode) Settings::save();
    g_dragMode = 0;
    g_dragging = false;
}

void rebuildFontsAndInvalidate() {
    const AppSettings &s = Settings::get();
    const int size = std::clamp(s.editorFontSize, 8, 32); // never build a 0-height font
    if (g_app.mono) DeleteObject(g_app.mono);
    if (g_app.monoBold) DeleteObject(g_app.monoBold);
    if (g_app.uiFont) DeleteObject(g_app.uiFont);
    g_app.mono =
        CreateFontA(size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Cascadia Mono");
    g_app.monoBold =
        CreateFontA(size, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Cascadia Mono");
    g_app.uiFont =
        CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    g_app.editor.setFont(g_app.mono);
    g_app.hex.setFont(g_app.mono);
    g_app.disasm.setFont(g_app.mono);
    g_app.term.setFont(g_app.mono);
    g_app.regs.setFont(g_app.mono, g_app.monoBold);
    SendMessageA(g_app.cmdInput, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.mono), TRUE);
    if (g_app.mainWnd) {
        uiApplyLayout();
        applyThemeToWindow(g_app.mainWnd);
    }
}

void refreshRecentMenu(HMENU menu) {
    while (DeleteMenu(menu, 0, MF_BYPOSITION)) {
    }
    const auto &recent = Settings::get().recentFiles;
    if (recent.empty()) {
        AppendMenuA(menu, MF_STRING | MF_DISABLED, ID_RECENT_BASE, "(empty)");
        return;
    }
    // Numbered accelerators: &1..&9 then &0 for the tenth entry (item 99).
    static constexpr char kNumberKeys[10] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};
    int idx = 0;
    for (const auto &path : recent) {
        char label[MAX_PATH * 2];
        std::snprintf(label, sizeof(label), "&%c %s", kNumberKeys[idx], path.c_str());
        AppendMenuA(menu, MF_STRING, ID_RECENT_BASE + idx, label);
        if (++idx >= 10) break;
    }
}

namespace {

struct PaletteCmd {
    const char *label;
    CommandId id;
};

const PaletteCmd kPalette[] = {
    {"Build (F7)", ID_BUILD_BUILD},
    {"Burn and Run (F5)", ID_BUILD_BURNRUN},
    {"Upload to serial", ID_BUILD_UPLOAD},
    {"Run / Pause (F9)", ID_DEBUG_RUN},
    {"Step (F8)", ID_DEBUG_STEP},
    {"Reset", ID_DEBUG_RESET},
    {"Step back", ID_DEBUG_BACK},
    {"Step forward", ID_DEBUG_FORWARD},
    {"Toggle breakpoint at cursor", ID_DEBUG_TOGGLEBP},
    {"Run to cursor", ID_DEBUG_RUNCURSOR},
    {"Clear all breakpoints", ID_DEBUG_CLEARBP},
    {"New file", ID_FILE_NEW},
    {"Open file...", ID_FILE_OPEN},
    {"Save", ID_FILE_SAVE},
    {"Save as...", ID_FILE_SAVEAS},
    {"Undo", ID_EDIT_UNDO},
    {"Redo", ID_EDIT_REDO},
    {"Duplicate line", ID_EDIT_DUP},
    {"Toggle comment", ID_EDIT_COMMENT},
    {"Go to line...", ID_EDIT_GOTO},
    {"Find...", ID_EDIT_FIND},
    {"Find in files...", ID_EDIT_FINDINFILES},
    {"Screenshot", ID_VIEW_SCREENSHOT},
    {"Next theme", ID_VIEW_THEME_NEXT},
    {"Palette editor...", ID_VIEW_PALETTE_EDITOR},
    {"Settings...", ID_VIEW_SETTINGS},
    {"Self test", ID_TOOL_SELFTEST},
    {"Diff since snapshot", ID_TOOL_DIFF},
    {"About", ID_HELP_ABOUT},
};

HWND g_palette = nullptr;
HWND g_paletteList = nullptr;
WNDPROC g_oldPaletteEdit = nullptr;

// ---- Fuzzy command matching with recency weighting (item 100) ----

std::deque<int> g_recentCmds;

void noteCommandExecuted(int id) {
    for (auto it = g_recentCmds.begin(); it != g_recentCmds.end();) {
        if (*it == id)
            it = g_recentCmds.erase(it);
        else
            ++it;
    }
    g_recentCmds.push_back(id);
    while (g_recentCmds.size() > 10) g_recentCmds.pop_front();
}

int recencyBoost(int id) {
    int distanceFromNewest = 0;
    for (auto it = g_recentCmds.rbegin(); it != g_recentCmds.rend(); ++it, ++distanceFromNewest)
        if (*it == id) return std::max(0, 30 - 3 * distanceFromNewest);
    return 0;
}

bool isWordBoundaryChar(char prev) {
    return prev == ' ' || prev == '\t' || prev == '(' || prev == '[' || prev == '/' || prev == '&' ||
           prev == '.' || prev == '-' || prev == '_';
}

// Case-insensitive subsequence match scored with consecutive-char bonus,
// word-start bonus and gap penalty; -1 when query is not a subsequence.
int fuzzyScore(const char *text, const std::string &query) {
    const std::size_t tlen = std::strlen(text);
    int score = 0;
    std::size_t ti = 0;
    int prevMatch = -1;
    bool first = true;
    for (const char rawQ : query) {
        if (rawQ == ' ') continue;
        const char q = static_cast<char>(std::tolower(static_cast<unsigned char>(rawQ)));
        bool matched = false;
        for (; ti < tlen; ++ti) {
            const char tc = static_cast<char>(std::tolower(static_cast<unsigned char>(text[ti])));
            if (tc != q) continue;
            if (!first) {
                if (prevMatch >= 0 && static_cast<int>(ti) == prevMatch + 1)
                    score += 8; // consecutive characters
                else
                    score -= std::min(4, static_cast<int>(ti) - prevMatch - 1); // gap penalty
            }
            score += (ti > 0 && isWordBoundaryChar(text[ti - 1])) ? 10 : 2; // word-start bonus
            prevMatch = static_cast<int>(ti);
            ++ti;
            first = false;
            matched = true;
            break;
        }
        if (!matched) return -1;
    }
    return score;
}

void executePaletteSelection(HWND list, int sel) {
    if (sel < 0) return;
    const CommandId id = static_cast<CommandId>(SendMessageA(list, LB_GETITEMDATA, sel, 0));
    ShowWindow(g_palette, SW_HIDE);
    SetFocus(g_app.mainWnd);
    noteCommandExecuted(static_cast<int>(id));
    PostMessageA(g_app.mainWnd, WM_COMMAND, id, 0);
}

void refreshPaletteList(const char *filterText) {
    SendMessageA(g_paletteList, LB_RESETCONTENT, 0, 0);
    struct Ranked {
        int score;
        const PaletteCmd *cmd;
    };
    std::vector<Ranked> ranked;
    if (filterText[0] == 0) {
        for (const auto &cmd : kPalette)
            ranked.push_back({recencyBoost(static_cast<int>(cmd.id)), &cmd});
    } else {
        const std::string query = toLowerCopy(filterText);
        for (const auto &cmd : kPalette) {
            const int score = fuzzyScore(cmd.label, query);
            if (score >= 0) ranked.push_back({score + recencyBoost(static_cast<int>(cmd.id)), &cmd});
        }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const Ranked &a, const Ranked &b) { return a.score > b.score; });
        if (ranked.size() > 10) ranked.resize(10); // top 10 matches only
    }
    for (const auto &r : ranked) {
        const int idx = static_cast<int>(
            SendMessageA(g_paletteList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(r.cmd->label)));
        SendMessageA(g_paletteList, LB_SETITEMDATA, idx, static_cast<LPARAM>(r.cmd->id));
    }
    SendMessageA(g_paletteList, LB_SETCURSEL, 0, 0);
}

LRESULT CALLBACK paletteEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        switch (wParam) {
        case VK_ESCAPE:
            ShowWindow(g_palette, SW_HIDE);
            SetFocus(g_app.mainWnd);
            return 0;
        case VK_RETURN: {
            const int sel = static_cast<int>(SendMessageA(g_paletteList, LB_GETCURSEL, 0, 0));
            executePaletteSelection(g_paletteList, sel);
            return 0;
        }
        case VK_DOWN:
        case VK_UP: {
            int sel = static_cast<int>(SendMessageA(g_paletteList, LB_GETCURSEL, 0, 0));
            const int count = static_cast<int>(SendMessageA(g_paletteList, LB_GETCOUNT, 0, 0));
            sel = wParam == VK_DOWN ? std::min(count - 1, sel + 1) : std::max(0, sel - 1);
            SendMessageA(g_paletteList, LB_SETCURSEL, sel, 0);
            return 0;
        }
        default:
            break;
        }
    }
    return CallWindowProcA(g_oldPaletteEdit, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK paletteListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_LBUTTONDBLCLK) {
        const int sel = static_cast<int>(SendMessageA(hwnd, LB_GETCURSEL, 0, 0));
        executePaletteSelection(hwnd, sel);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK paletteProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        winutil::fillRectColor(dc, rc, theme().panelBg);
        winutil::frameRectColor(dc, rc, theme().border);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        // Owner-draw notifications for the list box arrive HERE (the parent),
        // not in the list box's own wndproc - painting them in the subclass
        // left every item blank.
        auto *dis = reinterpret_cast<DRAWITEMSTRUCT *>(lParam);
        if (dis->CtlType != ODT_LISTBOX) break;
        const Theme &t = theme();
        winutil::fillRectColor(dis->hDC, dis->rcItem,
                               dis->itemState & ODS_SELECTED ? t.selectionBg : t.panelBg);
        char buf[128]{};
        SendMessageA(dis->hwndItem, LB_GETTEXT, dis->itemID, reinterpret_cast<LPARAM>(buf));
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, dis->itemState & ODS_SELECTED ? t.selectionFg : t.fg);
        TextOutA(dis->hDC, dis->rcItem.left + 8, dis->rcItem.top + 2, buf, static_cast<int>(strlen(buf)));
        return TRUE;
    }
    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE && lParam) {
            char filter[128]{};
            GetWindowTextA(reinterpret_cast<HWND>(lParam), filter, sizeof(filter));
            refreshPaletteList(filter);
        }
        return 0;
    case WM_KILLFOCUS: {
        // SetFocus() from this popup to its own edit box fires WM_KILLFOCUS
        // here first; hiding in that case slammed the palette shut the
        // instant it opened. Only close when focus leaves the popup tree.
        const HWND focused = GetFocus();
        if (!focused || GetAncestor(focused, GA_ROOT) != hwnd) ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

} // namespace

void paletteToggle(HWND owner) {
    if (!g_palette) {
        WNDCLASSA wc{};
        wc.lpfnWndProc = paletteProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "6502Palette";
        RegisterClassA(&wc);
        g_palette = CreateWindowExA(WS_EX_TOOLWINDOW, "6502Palette", nullptr, WS_POPUP | WS_BORDER, 100, 100,
                                    420, 360, owner, nullptr, GetModuleHandleA(nullptr), nullptr);
        HWND edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_WANTRETURN, 8, 8,
                                    404, 28, g_palette, reinterpret_cast<HMENU>(static_cast<INT_PTR>(900)),
                                    GetModuleHandleA(nullptr), nullptr);
        g_oldPaletteEdit = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(paletteEditProc)));
        g_paletteList = CreateWindowExA(
            0, "LISTBOX", "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
            8, 42, 404, 300, g_palette, reinterpret_cast<HMENU>(static_cast<INT_PTR>(901)),
            GetModuleHandleA(nullptr), nullptr);
        SetWindowLongPtrA(g_paletteList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(paletteListProc));
    }
    if (!g_paletteList) return;
    SendMessageA(GetDlgItem(g_palette, 900), WM_SETFONT, reinterpret_cast<WPARAM>(g_app.uiFont), TRUE);
    SendMessageA(g_paletteList, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.uiFont), TRUE);

    if (IsWindowVisible(g_palette)) {
        ShowWindow(g_palette, SW_HIDE);
        SetFocus(owner);
        return;
    }
    RECT rcOwner{};
    GetWindowRect(owner, &rcOwner);
    SetWindowPos(g_palette, HWND_TOPMOST, rcOwner.left + 220, rcOwner.top + 44, 420, 360, SWP_SHOWWINDOW);
    SetFocus(GetDlgItem(g_palette, 900));
    SendMessageA(GetDlgItem(g_palette, 900), EM_SETSEL, 0, -1);
}

bool paletteIsVisible() {
    return g_palette && IsWindowVisible(g_palette);
}

namespace {

HWND g_findBar = nullptr;
HWND g_findEdit = nullptr;
bool g_findIsGoto = false;
WNDPROC g_oldFindEdit = nullptr;

LRESULT CALLBACK findEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) {
            ShowWindow(g_findBar, SW_HIDE);
            SetFocus(g_app.editor.hwnd());
            return 0;
        }
        if (wParam == VK_RETURN) {
            char buf[256]{};
            GetWindowTextA(hwnd, buf, sizeof(buf));
            ShowWindow(g_findBar, SW_HIDE);
            SetFocus(g_app.editor.hwnd());
            if (g_findIsGoto)
                PostMessageA(g_app.mainWnd, WM_APP + 45, 2, static_cast<LPARAM>(std::atoi(buf)));
            else
                PostMessageA(g_app.mainWnd, WM_APP + 45, 1, reinterpret_cast<LPARAM>(new std::string(buf)));
            return 0;
        }
    }
    return CallWindowProcA(g_oldFindEdit, hwnd, msg, wParam, lParam);
}

} // namespace

const std::string &findBarText() {
    static std::string text;
    return text;
}

void findBarToggle(HWND owner, bool gotoMode) {
    if (!g_findBar) {
        WNDCLASSA wc{};
        wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
            switch (msg) {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd, &ps);
                RECT rc{};
                GetClientRect(hwnd, &rc);
                winutil::fillRectColor(dc, rc, theme().toolbarBg);
                winutil::frameRectColor(dc, rc, theme().border);
                EndPaint(hwnd, &ps);
                return 0;
            }
            default:
                return DefWindowProcA(hwnd, msg, wParam, lParam);
            }
        };
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "6502FindBar";
        RegisterClassA(&wc);
        g_findBar = CreateWindowExA(WS_EX_TOOLWINDOW, "6502FindBar", nullptr, WS_POPUP | WS_BORDER, 100, 100,
                                    380, 46, owner, nullptr, GetModuleHandleA(nullptr), nullptr);
        CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_WANTRETURN, 8, 9, 250, 28,
                        g_findBar, reinterpret_cast<HMENU>(static_cast<INT_PTR>(910)),
                        GetModuleHandleA(nullptr), nullptr);
    }
    g_findEdit = GetDlgItem(g_findBar, 910);
    SendMessageA(g_findEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.uiFont), TRUE);

    if (IsWindowVisible(g_findBar)) {
        ShowWindow(g_findBar, SW_HIDE);
        SetFocus(owner);
        return;
    }
    g_findIsGoto = gotoMode;
    SetWindowTextA(g_findEdit, "");
    RECT rcOwner{};
    GetWindowRect(owner, &rcOwner);
    SetWindowPos(g_findBar, HWND_TOPMOST, rcOwner.right - 420, rcOwner.top + kToolH + 4, 390, 48,
                 SWP_SHOWWINDOW);
    g_oldFindEdit = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(g_findEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(findEditProc)));
    SetFocus(g_findEdit);
}

bool findBarVisible() {
    return g_findBar && IsWindowVisible(g_findBar);
}

// ---- Find in files (item 32) ----

namespace {

HWND g_fifBar = nullptr;
WNDPROC g_oldFifEdit = nullptr;
HWND g_modalSheet = nullptr;
HWND g_modalSheetOwner = nullptr;

void modalSheetClose() {
    if (g_modalSheetOwner) {
        EnableWindow(g_modalSheetOwner, TRUE);
        SetFocus(g_modalSheetOwner);
        g_modalSheetOwner = nullptr;
    }
}

void findInFilesRun(const std::string &needleText) {
    namespace fs = std::filesystem;
    if (needleText.empty()) return;
    const std::string needleLower = toLowerCopy(needleText);
    fs::path root = ".";
    if (!g_app.currentFile.empty()) {
        try {
            const fs::path p(g_app.currentFile);
            if (p.has_parent_path() && !p.parent_path().empty()) root = p.parent_path();
        } catch (...) {
        }
    }
    char head[512];
    std::snprintf(head, sizeof(head), "[FFIND] \"%s\" under %s\r\n", needleText.c_str(),
                  root.string().c_str());
    g_app.term.append(TermView::Role::Info, head);
    int hits = 0;
    constexpr int kMaxHits = 5000;
    try {
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied), end;
             it != end && hits < kMaxHits; ++it) {
            const fs::directory_entry &entry = *it;
            if (!entry.is_regular_file()) continue;
            const std::string ext = toLowerCopy(entry.path().extension().string());
            if (ext != ".asm" && ext != ".inc") continue;
            std::ifstream in(entry.path(), std::ios::binary);
            if (!in) continue;
            std::string line;
            int lineNo = 0;
            while (std::getline(in, line) && hits < kMaxHits) {
                ++lineNo;
                if (toLowerCopy(line).find(needleLower) == std::string::npos) continue;
                std::string text = line.substr(0, 140);
                for (char &c : text)
                    if (c == '\r' || c == '\t') c = ' ';
                char row[MAX_PATH + 180];
                std::snprintf(row, sizeof(row), "%s(%d): %s\r\n", entry.path().string().c_str(), lineNo,
                              text.c_str());
                g_app.term.append(TermView::Role::Info, row);
                ++hits;
            }
        }
    } catch (const std::exception &e) {
        g_app.term.append(TermView::Role::Warn, std::string("[FFIND] ") + e.what() + "\r\n");
    }
    if (hits >= kMaxHits) g_app.term.append(TermView::Role::Warn, "[FFIND] hit limit reached\r\n");
    char tail[64];
    std::snprintf(tail, sizeof(tail), "[FFIND] %d match%s\r\n", hits, hits == 1 ? "" : "es");
    g_app.term.append(hits > 0 ? TermView::Role::Ok : TermView::Role::Dim, tail);
}

LRESULT CALLBACK fifEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) {
            ShowWindow(g_fifBar, SW_HIDE);
            SetFocus(g_app.mainWnd);
            return 0;
        }
        if (wParam == VK_RETURN) {
            char buf[256]{};
            GetWindowTextA(hwnd, buf, sizeof(buf));
            ShowWindow(g_fifBar, SW_HIDE);
            SetFocus(g_app.mainWnd);
            findInFilesRun(buf);
            return 0;
        }
    }
    return CallWindowProcA(g_oldFifEdit, hwnd, msg, wParam, lParam);
}

} // namespace

void findInFilesToggle(HWND owner) {
    if (!g_fifBar) {
        WNDCLASSA wc{};
        wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
            switch (msg) {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd, &ps);
                RECT rc{};
                GetClientRect(hwnd, &rc);
                winutil::fillRectColor(dc, rc, theme().toolbarBg);
                winutil::frameRectColor(dc, rc, theme().border);
                SelectObject(dc, g_app.uiFont);
                SetBkMode(dc, TRANSPARENT);
                winutil::drawTextOut(dc, 10, 12, "Find in files:", theme().dimFg);
                EndPaint(hwnd, &ps);
                return 0;
            }
            default:
                return DefWindowProcA(hwnd, msg, wParam, lParam);
            }
        };
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "6502FindInFiles";
        RegisterClassA(&wc);
        g_fifBar = CreateWindowExA(WS_EX_TOOLWINDOW, "6502FindInFiles", nullptr, WS_POPUP | WS_BORDER, 100,
                                   100, 450, 48, owner, nullptr, GetModuleHandleA(nullptr), nullptr);
        CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_WANTRETURN, 112, 9, 326,
                        28, g_fifBar, reinterpret_cast<HMENU>(static_cast<INT_PTR>(920)),
                        GetModuleHandleA(nullptr), nullptr);
    }
    HWND edit = GetDlgItem(g_fifBar, 920);
    SendMessageA(edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.uiFont), TRUE);

    if (IsWindowVisible(g_fifBar)) {
        ShowWindow(g_fifBar, SW_HIDE);
        SetFocus(owner);
        return;
    }
    SetWindowTextA(edit, "");
    RECT rcOwner{};
    GetWindowRect(owner, &rcOwner);
    SetWindowPos(g_fifBar, HWND_TOPMOST, rcOwner.left + 120, rcOwner.top + kToolH + 4, 450, 48,
                 SWP_SHOWWINDOW);
    g_oldFifEdit =
        reinterpret_cast<WNDPROC>(SetWindowLongPtrA(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(fifEditProc)));
    SetFocus(edit);
}

// ---- Settings dialog (item 36) ----

namespace {

constexpr LONG kSetTheme = 1000;
constexpr LONG kSetFont = 1001;
constexpr LONG kSetSpin = 1002;
constexpr LONG kSetSpeed = 1003;
constexpr LONG kSetCpuCore = 1004;
constexpr LONG kSetAutoSave = 1005;
constexpr LONG kSetSmooth = 1006;
constexpr LONG kSetTurbo = 1007;
constexpr LONG kSetAutoRebuild = 1008;
constexpr LONG kSetIncludes = 1009;
constexpr LONG kSetDefines = 1010;
constexpr LONG kSetWarn = 1011;

void settingsApply(HWND dlg) {
    AppSettings &s = Settings::get();
    auto checked = [&](LONG id) {
        return SendMessageA(GetDlgItem(dlg, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
    };
    const int themeSel = static_cast<int>(SendMessageA(GetDlgItem(dlg, kSetTheme), CB_GETCURSEL, 0, 0));
    if (themeSel >= 0 && themeSel < themeCount()) {
        s.themeIndex = themeSel;
        setThemeIndex(themeSel);
    }
    // Read the font size from the spinner (buddy-synced with the edit box)
    // so an out-of-range or empty edit can never smuggle in a bogus value.
    const int fontSize =
        static_cast<int>(SendMessageA(GetDlgItem(dlg, kSetSpin), UDM_GETPOS32, 0, 0));
    s.editorFontSize = std::clamp(fontSize, 8, 32);
    SetDlgItemTextA(dlg, kSetFont, std::to_string(s.editorFontSize).c_str());
    char buf[512]{};
    GetDlgItemTextA(dlg, kSetSpeed, buf, sizeof(buf));
    s.speed = static_cast<uint32_t>(std::clamp<long>(std::atol(buf), 100, 50000000));
    const bool c02 = checked(kSetCpuCore);
    s.cpuCore = c02 ? 1 : 0;
    g_app.emu.setCoreVariant(c02 ? CPU::Core::WDC65C02 : CPU::Core::NMOS6502);
    s.autoSave = checked(kSetAutoSave);
    s.smoothScreen = checked(kSetSmooth);
    g_app.disp.setSmoothScaling(s.smoothScreen);
    s.turboEnabled = checked(kSetTurbo);
    s.autoRebuild = checked(kSetAutoRebuild);
    GetDlgItemTextA(dlg, kSetIncludes, buf, sizeof(buf));
    s.includePaths = buf;
    GetDlgItemTextA(dlg, kSetDefines, buf, sizeof(buf));
    s.predefinedDefines = buf;
    static constexpr int kWarnValues[] = {0, 1, 3, 7};
    const int warnSel = static_cast<int>(SendMessageA(GetDlgItem(dlg, kSetWarn), CB_GETCURSEL, 0, 0));
    if (warnSel >= 0 && warnSel < 4) s.warnLevel = kWarnValues[warnSel];
    Settings::save();
    rebuildFontsAndInvalidate();
    applyThemeToWindow(g_app.mainWnd);
    osdToast("Settings applied");
}

LRESULT CALLBACK settingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        winutil::fillRectColor(dc, rc, theme().panelBg);
        return 1;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 1 || LOWORD(wParam) == 2) { // OK / Cancel
            if (LOWORD(wParam) == 1) settingsApply(hwnd);
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY:
        g_modalSheet = nullptr;
        modalSheetClose();
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

} // namespace

HWND uiModalSheetWindow() {
    return g_modalSheet;
}

void settingsDialogShow(HWND owner) {
    if (g_modalSheet) {
        SetFocus(g_modalSheet);
        return;
    }
    WNDCLASSA wc{};
    wc.lpfnWndProc = settingsProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = "6502SettingsDlg";
    RegisterClassA(&wc);

    RECT rcOwner{};
    GetWindowRect(owner, &rcOwner);
    HWND dlg =
        CreateWindowExA(WS_EX_DLGMODALFRAME, "6502SettingsDlg", "Settings", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                        rcOwner.left + 120, rcOwner.top + 60, 480, 500, owner, nullptr,
                        GetModuleHandleA(nullptr), nullptr);
    if (!dlg) return;
    g_modalSheet = dlg;
    g_modalSheetOwner = owner;
    EnableWindow(owner, FALSE);

    const AppSettings &s = Settings::get();
    HINSTANCE hi = GetModuleHandleA(nullptr);
    auto label = [&](const char *text, int x, int y, int w) {
        CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, 18, dlg,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(-1)), hi, nullptr);
    };
    auto check = [&](const char *text, LONG id, int y, bool on) {
        HWND c = CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 16,
                                 y, 320, 20, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hi,
                                 nullptr);
        SendMessageA(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    };
    auto editBox = [&](const char *initial, LONG id, int x, int y, int w) {
        CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", initial,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, x, y, w, 24, dlg,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hi, nullptr);
    };

    label("Theme:", 16, 18, 90);
    HWND themeCombo = CreateWindowExA(WS_EX_CLIENTEDGE, "COMBOBOX", "",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 110, 14, 200,
                                      200, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSetTheme)),
                                      hi, nullptr);
    for (int i = 0; i < themeCount(); ++i)
        SendMessageA(themeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(themeName(i)));
    SendMessageA(themeCombo, CB_SETCURSEL,
                 static_cast<WPARAM>(((s.themeIndex % themeCount()) + themeCount()) % themeCount()), 0);

    label("Font size (8-32):", 16, 52, 130);
    editBox(std::to_string(s.editorFontSize).c_str(), kSetFont, 150, 48, 60);
    CreateWindowExA(
        0, UPDOWN_CLASSA, "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS |
            UDS_AUTOBUDDY | UDS_NOTHOUSANDS,
        210, 48, 16, 24, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSetSpin)), hi, nullptr);
    SendMessageA(GetDlgItem(dlg, kSetSpin), UDM_SETRANGE32, 8, 32);
    // Sync the spinner to the saved value AFTER setting its range; otherwise
    // the buddy edit box gets overwritten with the spinner's default
    // position (0), which later clamps the font to 8 and shrinks all text.
    SendMessageA(GetDlgItem(dlg, kSetSpin), UDM_SETPOS32, 0, s.editorFontSize);

    label("Speed (instructions/tick):", 16, 86, 180);
    editBox(std::to_string(static_cast<unsigned long long>(s.speed)).c_str(), kSetSpeed, 200, 82, 160);

    check("65C02 core (WDC)", kSetCpuCore, 122, s.cpuCore != 0);
    check("Auto save", kSetAutoSave, 148, s.autoSave);
    check("Smooth screen scaling", kSetSmooth, 174, s.smoothScreen);
    check("Turbo fast-forward (hold Tab)", kSetTurbo, 200, s.turboEnabled);
    check("Rebuild automatically on save (.asm)", kSetAutoRebuild, 226, s.autoRebuild);

    label("Include paths (';' separated):", 16, 258, 300);
    editBox(s.includePaths.c_str(), kSetIncludes, 16, 276, 442);
    label("Predefined defines (NAME=value,...):", 16, 308, 300);
    editBox(s.predefinedDefines.c_str(), kSetDefines, 16, 326, 442);

    label("Warning level:", 16, 360, 130);
    HWND warnCombo = CreateWindowExA(WS_EX_CLIENTEDGE, "COMBOBOX", "",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 150, 356, 160,
                                     200, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSetWarn)), hi,
                                     nullptr);
    static constexpr int kWarnValues[] = {0, 1, 3, 7};
    static constexpr const char *kWarnLabels[] = {"None", "Generic", "Normal", "All"};
    int warnSel = 2;
    for (int i = 0; i < 4; ++i) {
        SendMessageA(warnCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kWarnLabels[i]));
        if (kWarnValues[i] == s.warnLevel) warnSel = i;
    }
    SendMessageA(warnCombo, CB_SETCURSEL, static_cast<WPARAM>(warnSel), 0);

    CreateWindowExA(0, "BUTTON", "OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 296, 420, 78,
                    28, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)), hi, nullptr);
    CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 380, 420, 78,
                    28, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(2)), hi, nullptr);

    EnumChildWindows(
        dlg,
        [](HWND child, LPARAM fontParam) -> BOOL {
            SendMessageA(child, WM_SETFONT, static_cast<WPARAM>(fontParam), TRUE);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(g_app.uiFont));

    ShowWindow(dlg, SW_SHOW);
    SetFocus(themeCombo);
}

// ---- Savestate diff viewer (item 49) ----

namespace {

HWND g_diffWnd = nullptr;
HWND g_diffList = nullptr;

LRESULT CALLBACK diffWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_diffList)
            MoveWindow(g_diffList, 8, 8, LOWORD(lParam) - 16, HIWORD(lParam) - 16, TRUE);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY:
        g_diffWnd = nullptr;
        g_diffList = nullptr;
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

} // namespace

void snapshotDiffViewerShow(HWND owner) {
    if (!g_app.dbg.hasSnapshot()) {
        osdToast("No snapshot taken");
        return;
    }
    if (!g_diffWnd) {
        WNDCLASSA wc{};
        wc.lpfnWndProc = diffWndProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        wc.lpszClassName = "6502DiffView";
        RegisterClassA(&wc);
        RECT rcOwner{};
        GetWindowRect(owner, &rcOwner);
        g_diffWnd = CreateWindowExA(0, "6502DiffView", "Diff since snapshot",
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                    rcOwner.left + 180, rcOwner.top + 120, 560, 520, owner, nullptr,
                                    GetModuleHandleA(nullptr), nullptr);
        g_diffList = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT |
                                         LBS_HASSTRINGS,
                                     8, 8, 530, 460, g_diffWnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(930)),
                                     GetModuleHandleA(nullptr), nullptr);
    }
    if (!g_diffList) return;
    SendMessageA(g_diffList, LB_RESETCONTENT, 0, 0);
    const std::vector<DebugMemoryDelta> rows = g_app.dbg.snapshotDiff();
    constexpr std::size_t kMaxRows = 2000;
    char line[96];
    std::snprintf(line, sizeof(line), "%zu changed byte%s since snapshot", rows.size(),
                  rows.size() == 1 ? "" : "s");
    SendMessageA(g_diffList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line));
    std::size_t shown = 0;
    for (const auto &row : rows) {
        if (shown >= kMaxRows) {
            std::snprintf(line, sizeof(line), "... +%zu more (display capped at %zu)",
                          rows.size() - kMaxRows, kMaxRows);
            SendMessageA(g_diffList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line));
            break;
        }
        std::snprintf(line, sizeof(line), "$%04X  %02X -> %02X", static_cast<unsigned>(row.address),
                      static_cast<unsigned>(row.oldValue), static_cast<unsigned>(row.newValue));
        SendMessageA(g_diffList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line));
        ++shown;
    }
    SendMessageA(g_diffList, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.mono), TRUE);
    ShowWindow(g_diffWnd, SW_SHOWNORMAL);
    SetFocus(g_diffList);
}

// ---- Display palette editor (item 35) ----

namespace {

constexpr int kCellSize = 22;
constexpr int kGridPad = 12;
constexpr LONG kPeR = 1100;
constexpr LONG kPeG = 1101;
constexpr LONG kPeB = 1102;
constexpr LONG kPeApply = 1103;
constexpr LONG kPeReset = 1104;
constexpr LONG kPeIndex = 1105;

HWND g_palEd = nullptr;
int g_palEdSel = 0;

void palEdLoadRgb(HWND dlg) {
    const uint32_t argb = g_app.disp.paletteEntry(g_palEdSel);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(argb & 0xFF));
    SetWindowTextA(GetDlgItem(dlg, kPeR), buf);
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>((argb >> 8) & 0xFF));
    SetWindowTextA(GetDlgItem(dlg, kPeG), buf);
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>((argb >> 16) & 0xFF));
    SetWindowTextA(GetDlgItem(dlg, kPeB), buf);
    char idxLabel[32];
    std::snprintf(idxLabel, sizeof(idxLabel), "Color #%d (%02X)", g_palEdSel, g_palEdSel);
    SetWindowTextA(GetDlgItem(dlg, kPeIndex), idxLabel);
}

void palEdApply(HWND dlg) {
    auto channel = [&](LONG id) {
        char buf[16]{};
        GetDlgItemTextA(dlg, id, buf, sizeof(buf));
        return static_cast<uint32_t>(std::clamp(std::atoi(buf), 0, 255));
    };
    const uint32_t r = channel(kPeR);
    const uint32_t g = channel(kPeG);
    const uint32_t b = channel(kPeB);
    g_app.disp.setPaletteEntry(g_palEdSel, 0xFF000000u | (b << 16) | (g << 8) | r);
    InvalidateRect(dlg, nullptr, FALSE);
}

LRESULT CALLBACK paletteEditorProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        winutil::fillRectColor(dc, rc, theme().panelBg);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        const Theme &t = theme();
        SelectObject(dc, g_app.uiFont);
        SetBkMode(dc, TRANSPARENT);
        HPEN pen = CreatePen(PS_SOLID, 1, t.border);
        HPEN oldPen = static_cast<HPEN>(SelectObject(dc, pen));
        for (int i = 0; i < 256; ++i) {
            const LONG x = kGridPad + (i % 16) * kCellSize;
            const LONG y = kGridPad + (i / 16) * kCellSize;
            RECT cell{x, y, x + kCellSize, y + kCellSize};
            const uint32_t argb = g_app.disp.paletteEntry(i);
            HBRUSH brush =
                CreateSolidBrush(RGB(argb & 0xFF, (argb >> 8) & 0xFF, (argb >> 16) & 0xFF));
            FillRect(dc, &cell, brush);
            DeleteObject(brush);
            FrameRect(dc, &cell, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        RECT selCell{kGridPad + (g_palEdSel % 16) * kCellSize - 1,
                     kGridPad + (g_palEdSel / 16) * kCellSize - 1,
                     kGridPad + (g_palEdSel % 16) * kCellSize + kCellSize + 1,
                     kGridPad + (g_palEdSel / 16) * kCellSize + kCellSize + 1};
        HPEN accentPen = CreatePen(PS_SOLID, 2, t.accent);
        oldPen = static_cast<HPEN>(SelectObject(dc, accentPen));
        HBRUSH nullBrush = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
        HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, nullBrush));
        Rectangle(dc, selCell.left, selCell.top, selCell.right, selCell.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(accentPen);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lParam) - kGridPad;
        const int y = GET_Y_LPARAM(lParam) - kGridPad;
        if (x >= 0 && y >= 0 && x < kCellSize * 16 && y < kCellSize * 16) {
            g_palEdSel = (y / kCellSize) * 16 + (x / kCellSize);
            palEdLoadRgb(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == kPeApply) {
            palEdApply(hwnd);
        } else if (LOWORD(wParam) == kPeReset) {
            g_app.disp.resetPalette();
            palEdLoadRgb(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_KILLFOCUS: {
        const HWND focused = GetFocus();
        if (!focused || GetAncestor(focused, GA_ROOT) != hwnd) ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    case WM_NCDESTROY:
        g_palEd = nullptr;
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

} // namespace

void paletteEditorToggle(HWND owner) {
    if (!g_palEd) {
        WNDCLASSA wc{};
        wc.lpfnWndProc = paletteEditorProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        wc.lpszClassName = "6502PalEdit";
        RegisterClassA(&wc);
        RECT rcOwner{};
        GetWindowRect(owner, &rcOwner);
        const int gridPx = kCellSize * 16 + kGridPad * 2;
        g_palEd = CreateWindowExA(WS_EX_DLGMODALFRAME, "6502PalEdit", "Palette Editor",
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                  rcOwner.right - gridPx - 60, rcOwner.top + 80, gridPx + 16,
                                  gridPx + 130, owner, nullptr, GetModuleHandleA(nullptr), nullptr);
        if (!g_palEd) return;
        HINSTANCE hi = GetModuleHandleA(nullptr);
        CreateWindowExA(0, "STATIC", "Color #0 (00)", WS_CHILD | WS_VISIBLE, kGridPad,
                        gridPx + 6, 160, 18, g_palEd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPeIndex)), hi, nullptr);
        const char *names[3] = {"R", "G", "B"};
        for (int i = 0; i < 3; ++i) {
            char labelId[8];
            std::snprintf(labelId, sizeof(labelId), "%s:", names[i]);
            CreateWindowExA(0, "STATIC", labelId, WS_CHILD | WS_VISIBLE, kGridPad + 170 + i * 62,
                            gridPx + 6, 18, 18, g_palEd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(-1)), hi, nullptr);
            CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_NUMBER, kGridPad + 186 + i * 62,
                            gridPx + 4, 40, 22, g_palEd,
                            reinterpret_cast<HMENU>(
                                static_cast<INT_PTR>(kPeR + i)),
                            hi, nullptr);
        }
        CreateWindowExA(0, "BUTTON", "Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 290,
                        gridPx + 30, 74, 26, g_palEd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPeApply)), hi, nullptr);
        CreateWindowExA(0, "BUTTON", "Reset All", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, kGridPad,
                        gridPx + 30, 74, 26, g_palEd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPeReset)), hi, nullptr);
        EnumChildWindows(
            g_palEd,
            [](HWND child, LPARAM fontParam) -> BOOL {
                SendMessageA(child, WM_SETFONT, static_cast<WPARAM>(fontParam), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(g_app.uiFont));
        palEdLoadRgb(g_palEd);
    }
    if (!g_palEd) return;
    if (IsWindowVisible(g_palEd)) {
        ShowWindow(g_palEd, SW_HIDE);
        return;
    }
    palEdLoadRgb(g_palEd);
    ShowWindow(g_palEd, SW_SHOWNORMAL);
}

bool paletteEditorIsVisible() {
    return g_palEd && IsWindowVisible(g_palEd);
}
