#include "theme.hpp"

#include <array>
#include <unordered_map>

namespace {

Theme makeDark() {
    Theme t{};
    t.windowBg = RGB(24, 24, 30);
    t.panelBg = RGB(18, 18, 23);
    t.border = RGB(52, 52, 64);
    t.toolbarBg = RGB(28, 28, 36);
    t.toolbarHover = RGB(44, 44, 56);
    t.toolbarPressed = RGB(58, 58, 74);
    t.statusBg = RGB(16, 16, 20);
    t.statusFg = RGB(150, 152, 165);
    t.fg = RGB(226, 227, 235);
    t.dimFg = RGB(128, 130, 145);
    t.accent = RGB(86, 156, 214);
    t.accentDim = RGB(48, 92, 132);
    t.ok = RGB(106, 204, 120);
    t.warn = RGB(222, 176, 84);
    t.err = RGB(232, 96, 96);
    t.selectionBg = RGB(50, 78, 110);
    t.selectionFg = RGB(240, 240, 250);
    t.curLine = RGB(30, 32, 40);
    t.gutterBg = RGB(21, 21, 26);
    t.gutterFg = RGB(90, 92, 105);
    t.lineNoActive = RGB(200, 202, 215);

    t.tokComment = RGB(112, 118, 130);
    t.tokString = RGB(206, 145, 120);
    t.tokNumber = RGB(180, 160, 230);
    t.tokMnemonic = RGB(86, 170, 230);
    t.tokDirective = RGB(220, 150, 210);
    t.tokLabel = RGB(240, 190, 100);
    t.tokRegister = RGB(140, 220, 170);
    t.tokPunct = RGB(170, 172, 185);

    t.hexAddr = RGB(96, 140, 200);
    t.hexChanged = RGB(235, 190, 90);
    t.hexAscii = RGB(140, 142, 158);
    t.disasmBp = RGB(232, 96, 96);
    t.disasmCurrent = RGB(106, 204, 120);
    return t;
}

Theme makeLight() {
    Theme t{};
    t.windowBg = RGB(238, 240, 244);
    t.panelBg = RGB(250, 251, 253);
    t.border = RGB(198, 202, 212);
    t.toolbarBg = RGB(228, 231, 238);
    t.toolbarHover = RGB(208, 214, 228);
    t.toolbarPressed = RGB(188, 196, 214);
    t.statusBg = RGB(228, 230, 238);
    t.statusFg = RGB(80, 82, 96);
    t.fg = RGB(34, 36, 46);
    t.dimFg = RGB(120, 124, 138);
    t.accent = RGB(0, 102, 178);
    t.accentDim = RGB(120, 168, 210);
    t.ok = RGB(24, 140, 60);
    t.warn = RGB(178, 122, 10);
    t.err = RGB(198, 40, 40);
    t.selectionBg = RGB(174, 205, 236);
    t.selectionFg = RGB(10, 12, 20);
    t.curLine = RGB(240, 242, 247);
    t.gutterBg = RGB(234, 236, 242);
    t.gutterFg = RGB(130, 134, 148);
    t.lineNoActive = RGB(40, 42, 54);

    t.tokComment = RGB(120, 126, 140);
    t.tokString = RGB(162, 62, 30);
    t.tokNumber = RGB(94, 44, 165);
    t.tokMnemonic = RGB(0, 90, 158);
    t.tokDirective = RGB(150, 40, 140);
    t.tokLabel = RGB(146, 96, 10);
    t.tokRegister = RGB(20, 120, 70);
    t.tokPunct = RGB(70, 72, 84);

    t.hexAddr = RGB(40, 90, 170);
    t.hexChanged = RGB(190, 120, 10);
    t.hexAscii = RGB(110, 114, 128);
    t.disasmBp = RGB(198, 40, 40);
    t.disasmCurrent = RGB(24, 140, 60);
    return t;
}

Theme makeContrast() {
    Theme t = makeDark();
    t.windowBg = RGB(0, 0, 0);
    t.panelBg = RGB(0, 0, 0);
    t.toolbarBg = RGB(10, 10, 10);
    t.statusBg = RGB(0, 0, 0);
    t.fg = RGB(255, 255, 255);
    t.dimFg = RGB(180, 180, 180);
    t.border = RGB(120, 120, 120);
    t.curLine = RGB(40, 40, 40);
    t.tokComment = RGB(160, 160, 160);
    t.tokMnemonic = RGB(120, 200, 255);
    t.tokDirective = RGB(255, 160, 255);
    t.tokLabel = RGB(255, 220, 120);
    t.tokNumber = RGB(210, 190, 255);
    t.tokString = RGB(255, 170, 140);
    return t;
}

int active = 0;

std::array<Theme, 3> &themes() {
    static std::array<Theme, 3> list{makeDark(), makeLight(), makeContrast()};
    return list;
}

} // namespace

const Theme &theme() {
    return themes()[static_cast<std::size_t>(active)];
}

void setThemeIndex(int index) {
    active = index % static_cast<int>(themes().size());
}
int themeCount() {
    return static_cast<int>(themes().size());
}
const char *themeName(int index) {
    switch (index % themeCount()) {
    case 0:
        return "Dark";
    case 1:
        return "Light";
    default:
        return "High Contrast";
    }
}

COLORREF blend(COLORREF a, COLORREF b, double t) {
    auto ch = [t](uint8_t x, uint8_t y) -> uint8_t { return static_cast<uint8_t>(x + (y - x) * t); };
    return RGB(ch(static_cast<uint8_t>(a & 0xFF), static_cast<uint8_t>(b & 0xFF)),
               ch(static_cast<uint8_t>((a >> 8) & 0xFF), static_cast<uint8_t>((b >> 8) & 0xFF)),
               ch(static_cast<uint8_t>((a >> 16) & 0xFF), static_cast<uint8_t>((b >> 16) & 0xFF)));
}

HBRUSH themeBrush(COLORREF c) {
    return CreateSolidBrush(c);
}
