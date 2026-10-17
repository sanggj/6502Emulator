#include "app.hpp"
#include "assembler.hpp"
#include "cpu.hpp"
#include "feature_catalog.hpp"
#include "main_ui.hpp"
#include "platform.hpp"
#include "remote.hpp"
#include "selftest_roms.hpp"
#include "serial.hpp"
#include "settings.hpp"
#include "theme.hpp"
#include "win_util.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

App g_app;

void termInfo(const std::string &s) {
    g_app.term.append(TermView::Role::Info, s);
}
void termError(const std::string &s) {
    g_app.term.append(TermView::Role::Err, s);
}
void termOk(const std::string &s) {
    g_app.term.append(TermView::Role::Ok, s);
}

void setStatusBar(const std::string &text) {
    extern void statusBarSetText(const std::string &);
    statusBarSetText(text);
}

namespace {

std::string fileLeaf(const std::string &path) {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Build "line N: message" diagnostics from assembleAndBurn's message text
// into editor squiggle marks (item 8b).
std::vector<CodeEditor::ErrorMark> parseBuildErrorMarks(const std::string &msg) {
    std::vector<CodeEditor::ErrorMark> marks;
    std::size_t pos = 0;
    while (pos < msg.size() && marks.size() < 64) {
        std::size_t eol = msg.find('\n', pos);
        if (eol == std::string::npos) eol = msg.size();
        const std::string lineText = msg.substr(pos, eol - pos);
        pos = eol + 1;
        if (lineText.rfind("line ", 0) != 0) continue;
        const std::size_t colon = lineText.find(':', 5);
        if (colon == std::string::npos) continue;
        try {
            CodeEditor::ErrorMark mark;
            mark.line = std::max(0, std::stoi(lineText.substr(5, colon - 5)) - 1); // 1-based -> 0-based
            mark.column = 0;
            mark.message = lineText.substr(colon + 1);
            while (!mark.message.empty() && (mark.message.front() == ' ')) mark.message.erase(0, 1);
            marks.push_back(std::move(mark));
        } catch (...) {
        }
    }
    return marks;
}

bool endsWithAsm(const std::string &path) {
    if (path.size() < 4) return false;
    std::string tail = path.substr(path.size() - 4);
    for (char &c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return tail == ".asm";
}

// Item 94: rebuild automatically after an explicit save of a .asm file.
void maybeAutoRebuild() {
    const AppSettings &s = Settings::get();
    if (!s.autoRebuild || !endsWithAsm(g_app.currentFile)) return;
    appBurn(false);
}

} // namespace

// Drops a pending run-to-cursor one-shot breakpoint. Callers MUST hold
// remote::emulatorMutex(): it mutates the breakpoint vector (remote.cpp
// iterates it under that lock).
void appClearRunToCursor() {
    if (g_app.runToAddress >= 0 && g_app.runToBpOurs)
        g_app.dbg.removeBreakpoint(static_cast<uint16_t>(g_app.runToAddress));
    g_app.runToAddress = -1;
    g_app.runToBpOurs = false;
}

void appBurn(bool andRun) {
    std::string msg;
    const std::string source = g_app.editor.getText();
    AssembleOptions options;
    try {
        options.basePath =
            g_app.currentFile.empty() ? "." : std::filesystem::path(g_app.currentFile).parent_path().string();
    } catch (...) {
        options.basePath = ".";
    }
    bool burned = false;
    {
        // Emulator (pause/burn/start) and breakpoint-vector mutations happen
        // under emulatorMutex() - the lock covers them exactly as
        // klaus/nestest cover their whole run, and is released before UI.
        std::lock_guard<std::mutex> guard(remote::emulatorMutex());
        g_app.emu.pause();
        g_app.dbg.clear();
        burned = g_app.emu.assembleAndBurn(source, msg);
        if (burned && andRun) g_app.emu.start();
    }
    if (!burned) {
        g_app.term.append(TermView::Role::Err, "[BUILD]\r\n" + msg + "\r\n");
        setStatusBar("Build failed");
        g_app.editor.setErrorMarks(parseBuildErrorMarks(msg));
        osdToast("Build failed");
        applyThemeToWindow(g_app.mainWnd);
        return;
    }
    g_app.term.append(TermView::Role::Ok, "[BUILD] " + msg + "\r\n");
    setStatusBar(msg);
    g_app.editor.setErrorMarks({});
    osdToast("Burn OK");
    g_app.disasm.jumpToPc();
}

void appUpload() {
    const auto image = g_app.emu.program().flatImage();
    if (image.empty()) {
        termInfo("[UPLOAD] Build first.\r\n");
        return;
    }
    const AppSettings &s = Settings::get();
    std::string error;
    if (SerialUploader::upload(s.comPort, s.baud, image, error)) {
        termOk("[UPLOAD] " + error + "\r\n");
        setStatusBar(error);
    } else {
        termError("[UPLOAD ERROR] " + error + "\r\n");
        setStatusBar("Upload failed");
    }
}

void appStart() {
    g_app.emu.start();
    setStatusBar("Running");
}

void appPause() {
    {
        // Emulator/CPU and breakpoint-vector mutations happen under
        // emulatorMutex(): the remote worker steps the CPU and iterates the
        // breakpoint vector under that lock (remote.cpp).
        std::lock_guard<std::mutex> guard(remote::emulatorMutex());
        // A pending run-to-cursor must not leak its temporary breakpoint.
        appClearRunToCursor();
        g_app.emu.pause();
    }
    setStatusBar("Paused");
}

void appStepInstruction() {
    int cycles = 0;
    bool stopBp = false, stopWp = false;
    {
        std::lock_guard<std::mutex> guard(remote::emulatorMutex());
        cycles = g_app.dbg.step();
        stopBp = g_app.dbg.lastStopWasBreakpoint();
        stopWp = g_app.dbg.lastStopWasWatchpoint();
    }
    if (cycles == 0) {
        if (stopBp) {
            setStatusBar("Breakpoint hit");
            g_app.term.append(TermView::Role::Warn, "[BREAK] Breakpoint hit at PC.\r\n");
            osdToast("Breakpoint hit");
        } else if (stopWp) {
            setStatusBar("Watchpoint hit");
            g_app.term.append(TermView::Role::Warn, "[WATCH] Watchpoint hit.\r\n");
        } else {
            setStatusBar("CPU halted");
            g_app.term.append(TermView::Role::Err, "[CPU] Execution halted.\r\n");
        }
    }
}

void appReset() {
    {
        // Reset + breakpoint clear (and dropping any pending run-to) mutate
        // emulator/debugger state: same lock as appPause().
        std::lock_guard<std::mutex> guard(remote::emulatorMutex());
        appClearRunToCursor();
        g_app.emu.reset();
        g_app.dbg.clear();
    }
    setStatusBar("Reset");
}

void appStepBack() {
    bool rewound;
    {
        std::lock_guard<std::mutex> guard(remote::emulatorMutex());
        rewound = g_app.dbg.stepBack();
    }
    termInfo(rewound ? "rewound one instruction\r\n" : "no history to rewind\r\n");
}

void appStepForward() {
    bool replayed;
    {
        std::lock_guard<std::mutex> guard(remote::emulatorMutex());
        replayed = g_app.dbg.stepForward();
    }
    termInfo(replayed ? "replayed one instruction\r\n" : "no future history\r\n");
}

void appRunToSelected() {
    // Run-to-cursor targets the source line containing the editor caret. The
    // assembler records emitted addresses by source line, so directives or
    // comments without emitted bytes resolve to the next emitted source line.
    const int sourceLine = g_app.editor.caretLine() + 1; // assembler lines are 1-based
    const auto &lineAddresses = g_app.emu.program().lineAddresses;
    if (lineAddresses.empty()) {
        termError("[RUN TO CURSOR] Build the program first.\r\n");
        return;
    }
    auto it = lineAddresses.find(sourceLine);
    if (it == lineAddresses.end()) it = lineAddresses.lower_bound(sourceLine);
    if (it == lineAddresses.end()) {
        termError("[RUN TO CURSOR] No executable/emitted address at or after the cursor.\r\n");
        return;
    }
    const uint16_t addr = it->second;
    // Run-to-cursor as a ONE-SHOT breakpoint: the debugger stops exactly on
    // the target address. The scan of the breakpoint vector and the install
    // must happen under emulatorMutex() - the remote worker iterates the
    // same vector under that lock (remote.cpp).
    {
        std::lock_guard<std::mutex> guard(remote::emulatorMutex());
        // Drop any PREVIOUS run-to's one-shot breakpoint first, so a second
        // run-to cannot orphan the first (or mistake it for a user bp).
        appClearRunToCursor();
        bool userBpThere = false;
        for (const auto &bp : g_app.dbg.breakpoints())
            if (bp.address == addr) {
                userBpThere = true;
                break;
            }
        g_app.runToAddress = addr;
        g_app.runToBpOurs = !userBpThere;
        if (!userBpThere) g_app.dbg.addBreakpoint(addr);
        g_app.emu.start();
    }
    setStatusBar("Running to cursor");
}

void appToggleBreakpointAt(uint16_t address) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> guard(remote::emulatorMutex());
        for (const auto &bp : g_app.dbg.breakpoints())
            if (bp.address == address) {
                removed = true;
                break;
            }
        if (removed)
            g_app.dbg.removeBreakpoint(address);
        else
            g_app.dbg.addBreakpoint(address);
    }
    InvalidateRect(g_app.mainWnd, nullptr, FALSE);
}

bool appWriteCurrentToFile(const std::string &path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const std::string text = g_app.editor.getText();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) return false;
    g_app.currentFile = path;
    g_app.editor.clearModified();
    Settings::pushRecent(path);
    Settings::save();
    return true;
}

void appNewFile() {
    if (!appConfirmDiscard()) return;
    g_app.editor.setText("");
    g_app.currentFile.clear();
    setStatusBar("New file");
}

bool appConfirmDiscard() {
    if (!g_app.editor.modified()) return true;
    const int result = MessageBoxA(g_app.mainWnd, "Save changes to the current file?", "6502 IDE",
                                   MB_YESNOCANCEL | MB_ICONQUESTION);
    if (result == IDCANCEL) return false;
    if (result == IDYES && !g_app.currentFile.empty()) return appWriteCurrentToFile(g_app.currentFile);
    return true;
}

// Shared open-file code path: used by ID_FILE_OPEN and drag-and-drop (item 29).
void appOpenFileAtPath(const std::string &path) {
    if (!appConfirmDiscard()) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        termError("cannot open " + path + "\r\n");
        osdToast("Cannot open file");
        return;
    }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    g_app.editor.setText(content);
    g_app.currentFile = path;
    Settings::pushRecent(path);
    Settings::save();
    setStatusBar("Opened " + path);
    osdToast("Opened " + fileLeaf(path));
}

void appOpenFileDialog() {
    if (!appConfirmDiscard()) return;
    std::string path;
    if (!winutil::openFileDialog(g_app.mainWnd, path, "6502 Assembly\0*.asm;*.s;*.inc\0All files\0*.*\0"))
        return;
    appOpenFileAtPath(path);
}

void appSaveFileDialog(bool saveAs) {
    if (!saveAs && !g_app.currentFile.empty()) {
        if (!appWriteCurrentToFile(g_app.currentFile)) {
            termError("save failed\r\n");
            osdToast("Save failed");
        } else {
            setStatusBar("Saved " + g_app.currentFile);
            osdToast("Saved " + fileLeaf(g_app.currentFile));
            maybeAutoRebuild();
        }
        return;
    }
    std::string path;
    if (!winutil::saveFileDialog(g_app.mainWnd, path, "6502 Assembly\0*.asm\0All files\0*.*\0", "asm"))
        return;
    if (!appWriteCurrentToFile(path)) {
        termError("save failed\r\n");
        osdToast("Save failed");
    } else {
        setStatusBar("Saved " + path);
        osdToast("Saved " + fileLeaf(path));
        maybeAutoRebuild();
    }
}

void appScreenshot(const std::string &pathIn) {
    std::string path = pathIn;
    if (path.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "screen_%lld.bmp",
                      static_cast<long long>(std::chrono::system_clock::now().time_since_epoch().count()));
        // Portable mode: default captures land beside the .exe.
        path = platform::settingsDirectory() + "/" + buf;
    }
    std::string error;
    if (g_app.disp.saveScreenshot(path, error)) {
        termOk(error + "\r\n");
        osdToast("Screenshot saved");
    } else {
        termError(error + "\r\n");
        osdToast("Screenshot failed");
    }
}

void applyThemeToWindow(HWND root) {
    if (!root) return;
    EnumChildWindows(
        root,
        [](HWND hwnd, LPARAM) -> BOOL {
            InvalidateRect(hwnd, nullptr, FALSE);
            return TRUE;
        },
        0);
    InvalidateRect(root, nullptr, TRUE);
}

bool runSelfTests() {
    struct Case {
        const char *name;
        bool ok;
    };
    int failures = 0;
    auto check = [&](const char *name, bool ok) {
        if (!ok) ++failures;
        g_app.term.append(failures && !ok ? TermView::Role::Err : TermView::Role::Dim,
                          std::string(ok ? "  ok   " : "  FAIL ") + name + "\r\n");
    };

    uint16_t v = 0;
    check("expr basic", Assembler::parseValue("(2+3)*4", v) && v == 20);
    check("expr hi/lo",
          Assembler::parseValue("<$ABCD", v) && v == 0xCD && Assembler::parseValue(">$ABCD", v) && v == 0xAB);

    Assembler a;
    AssembledProgram prog;
    std::vector<AssemblerError> errs;
    const bool assembled =
        a.assemble(".org $8000\nlda #$05\nclc\nadc #3\nsta $0210\n.org $FFFC\n.word $8000\n", prog, errs);
    check("assemble sample", assembled && errs.empty());
    if (assembled) {
        Memory mem;
        for (int addr = 0x8000; addr <= 0x8010; ++addr)
            mem.write(static_cast<uint16_t>(addr), prog.memory[static_cast<std::size_t>(addr)]);
        mem.write(0xFFFC, 0x00);
        mem.write(0xFFFD, 0x80);
        CPU cpu(mem);
        cpu.setDecimalModeEnabled(false);
        cpu.reset();
        for (int i = 0; i < 4; ++i)
            if (cpu.step() == 0) break;
        check("cpu smoke run", cpu.A == 8 && mem.read(0x0210) == 8);
    }

    if (failures == 0)
        termOk("selftest: all passed\r\n");
    else
        termError("selftest: " + std::to_string(failures) + " failures\r\n");
    return failures == 0;
}

bool runKlausInEmulator(const std::string &path, std::string &report) {
    std::lock_guard<std::mutex> guard(remote::emulatorMutex());
    g_app.emu.pause();
    return klausFunctionalTest(g_app.emu.memory(), path, 100000000u, report);
}

bool runNestestInEmulator(const std::string &rom, const std::string &logFile, std::string &report) {
    std::lock_guard<std::mutex> guard(remote::emulatorMutex());
    g_app.emu.pause();
    return nestestValidate(g_app.emu, rom, logFile, 5000, report);
}