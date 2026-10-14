#pragma once

#include <string>

#include "capture.hpp"
#include "code_editor.hpp"
#include "debug_console.hpp"
#include "debugger.hpp"
#include "disasm_view.hpp"
#include "display.hpp"
#include "emulator.hpp"
#include "hex_view.hpp"
#include "regs_view.hpp"
#include "term_view.hpp"

struct App {
    Emulator emu;
    Debugger dbg{emu};
    Display disp;
    DebugConsole dbgConsole;

    CodeEditor editor;
    HexView hex;
    DisasmView disasm;
    RegsView regs;
    TermView term;

    HWND mainWnd = nullptr;
    HWND cmdInput = nullptr;

    HFONT mono = nullptr;
    HFONT monoBold = nullptr;
    HFONT uiFont = nullptr;

    std::string currentFile;
    bool closing = false;
    int runToAddress = -1;
    // True when runToAddress's breakpoint was placed by "run to cursor"
    // (and must be removed once reached), false when the user already had
    // a breakpoint at that address.
    bool runToBpOurs = false;

    // Frame capture / input replay state (driven from the terminal or UI).
    capture::GifWriter gif;
    capture::InputRecording input;
};

extern App g_app;

void appBurn(bool andRun);
void appUpload();
void appStart();
void appPause();
void appStepInstruction();
void appReset();
void appStepBack();
void appStepForward();
void appRunToSelected();
void appToggleBreakpointAt(uint16_t address);

bool appConfirmDiscard();
void appNewFile();
void appOpenFileDialog();
void appSaveFileDialog(bool saveAs);
bool appWriteCurrentToFile(const std::string &path);

void appScreenshot(const std::string &path);
void applyThemeToWindow(HWND root);
void setStatusBar(const std::string &text);
void termInfo(const std::string &s);
void termError(const std::string &s);
void termOk(const std::string &s);

void execTerminalCommand(std::string cmd);
bool runSelfTests();

static constexpr int APP_NOTIFY_EDITOR = 1;
