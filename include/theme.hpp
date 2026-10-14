#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>

struct Theme {
    COLORREF windowBg;
    COLORREF panelBg;
    COLORREF border;
    COLORREF toolbarBg;
    COLORREF toolbarHover;
    COLORREF toolbarPressed;
    COLORREF statusBg;
    COLORREF statusFg;
    COLORREF fg;
    COLORREF dimFg;
    COLORREF accent;
    COLORREF accentDim;
    COLORREF ok;
    COLORREF warn;
    COLORREF err;
    COLORREF selectionBg;
    COLORREF selectionFg;
    COLORREF curLine;
    COLORREF gutterBg;
    COLORREF gutterFg;
    COLORREF lineNoActive;

    COLORREF tokComment;
    COLORREF tokString;
    COLORREF tokNumber;
    COLORREF tokMnemonic;
    COLORREF tokDirective;
    COLORREF tokLabel;
    COLORREF tokRegister;
    COLORREF tokPunct;

    COLORREF hexAddr;
    COLORREF hexChanged;
    COLORREF hexAscii;
    COLORREF disasmBp;
    COLORREF disasmCurrent;
};

const Theme &theme();
void setThemeIndex(int index);
int themeCount();
const char *themeName(int index);
COLORREF blend(COLORREF a, COLORREF b, double t);
HBRUSH themeBrush(COLORREF c);
