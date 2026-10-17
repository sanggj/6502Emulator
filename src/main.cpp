#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>
#endif

#include "app.hpp"
#include "crash_handler.hpp"
#include "cpu.hpp"
#include "headless.hpp"
#include "main_ui.hpp"
#include "remote.hpp"
#include "settings.hpp"
#include "theme.hpp"
#include "win_util.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) static_cast<int>(static_cast<short>(LOWORD(lp)))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) static_cast<int>(static_cast<short>(HIWORD(lp)))
#endif

namespace {

constexpr char APP_CLASS[] = "6502EmulatorMainWindow";
constexpr int TIMER_ID = 1;
constexpr int TIMER_MS = 15;
constexpr int ID_FILE_EXIT = 2005;
constexpr int ID_DEBUG_QSAVE = 2312;
constexpr int ID_DEBUG_QLOAD = 2313;
constexpr int ID_VIEW_DEBUGCONSOLE = 2400;

HACCEL g_accel = nullptr;

} // namespace

// Defined in app_actions.cpp: shared open path for ID_FILE_OPEN and
// drag-and-drop (item 29).
void appOpenFileAtPath(const std::string &path);

namespace {

std::string defaultEditorSource() {
    return "; 6502 IDE - gradient example\r\n"
           "; Screen memory: $0200 (64x64)\r\n"
           ".org $8000\r\n\r\n"
           "start:\r\n"
           "    LDX #$00\r\n"
           "loop:\r\n"
           "    TXA\r\n"
           "    STA $0200,X\r\n"
           "    INX\r\n"
           "    BNE loop\r\n"
           "    JMP start\r\n\r\n"
           ".org $FFFC\r\n"
           ".word start\r\n";
}

WNDPROC g_oldInputProc = nullptr;

LRESULT CALLBACK inputProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        char buf[2048]{};
        GetWindowTextA(hwnd, buf, sizeof(buf));
        SetWindowTextA(hwnd, "");
        if (buf[0]) execTerminalCommand(buf);
        return 0;
    }
    return CallWindowProcA(g_oldInputProc, hwnd, msg, wParam, lParam);
}

void buildMenu(HWND hwnd) {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuA(file, MF_STRING, ID_FILE_NEW, "&New\tCtrl+N");
    AppendMenuA(file, MF_STRING, ID_FILE_OPEN, "&Open...\tCtrl+O");
    AppendMenuA(file, MF_STRING, ID_FILE_SAVE, "&Save\tCtrl+S");
    AppendMenuA(file, MF_STRING, ID_FILE_SAVEAS, "Save &As...\tCtrl+Shift+S");
    AppendMenuA(file, MF_SEPARATOR, 0, nullptr);
    HMENU recent = CreatePopupMenu();
    refreshRecentMenu(recent);
    AppendMenuA(file, MF_POPUP, reinterpret_cast<UINT_PTR>(recent), "Recent &Files");
    AppendMenuA(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(file, MF_STRING, ID_FILE_EXIT, "E&xit");
    AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), "&File");

    HMENU edit = CreatePopupMenu();
    AppendMenuA(edit, MF_STRING, ID_EDIT_UNDO, "&Undo\tCtrl+Z");
    AppendMenuA(edit, MF_STRING, ID_EDIT_REDO, "&Redo\tCtrl+Y");
    AppendMenuA(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(edit, MF_STRING, ID_EDIT_CUT, "Cu&t\tCtrl+X");
    AppendMenuA(edit, MF_STRING, ID_EDIT_COPY, "&Copy\tCtrl+C");
    AppendMenuA(edit, MF_STRING, ID_EDIT_PASTE, "&Paste\tCtrl+V");
    AppendMenuA(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(edit, MF_STRING, ID_EDIT_DUP, "&Duplicate Line\tCtrl+D");
    AppendMenuA(edit, MF_STRING, ID_EDIT_COMMENT, "Toggle &Comment\tCtrl+/");
    AppendMenuA(edit, MF_STRING, ID_EDIT_GOTO, "&Go To Line...\tCtrl+G");
    AppendMenuA(edit, MF_STRING, ID_EDIT_FIND, "&Find...\tCtrl+F");
    AppendMenuA(edit, MF_STRING, ID_EDIT_FINDINFILES, "Find in &Files...\tCtrl+Shift+F");
    AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), "&Edit");

    HMENU build = CreatePopupMenu();
    AppendMenuA(build, MF_STRING, ID_BUILD_BUILD, "&Build\tF7");
    AppendMenuA(build, MF_STRING, ID_BUILD_BURNRUN, "Build && &Run\tF5");
    AppendMenuA(build, MF_STRING, ID_BUILD_UPLOAD, "&Upload to COM");
    AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(build), "&Build");

    HMENU debug = CreatePopupMenu();
    AppendMenuA(debug, MF_STRING, ID_DEBUG_RUN, "&Run / Pause\tF9");
    AppendMenuA(debug, MF_STRING, ID_DEBUG_STEP, "St&ep\tF8");
    AppendMenuA(debug, MF_STRING, ID_DEBUG_RESET, "Rese&t\tCtrl+R");
    AppendMenuA(debug, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(debug, MF_STRING, ID_DEBUG_BACK, "Step &Back");
    AppendMenuA(debug, MF_STRING, ID_DEBUG_FORWARD, "Step &Forward");
    AppendMenuA(debug, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(debug, MF_STRING, ID_DEBUG_TOGGLEBP, "&Toggle Breakpoint at Cursor");
    AppendMenuA(debug, MF_STRING, ID_DEBUG_RUNCURSOR, "Run to &Cursor");
    AppendMenuA(debug, MF_STRING, ID_DEBUG_CLEARBP, "Clear All Breakpoints");
    AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(debug), "&Debug");

    HMENU view = CreatePopupMenu();
    AppendMenuA(view, MF_STRING, ID_VIEW_THEME_NEXT, "Next &Theme\tCtrl+T");
    AppendMenuA(view, MF_STRING, ID_VIEW_SCREENSHOT, "&Screenshot");
    AppendMenuA(view, MF_STRING, ID_VIEW_PALETTE, "Command &Palette\tCtrl+Shift+P");
    AppendMenuA(view, MF_STRING, ID_VIEW_PALETTE_EDITOR, "&Palette Editor...");
    AppendMenuA(view, MF_STRING, ID_VIEW_SETTINGS, "&Settings...\tCtrl+,");
    AppendMenuA(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(view, MF_STRING, ID_VIEW_DEBUGCONSOLE, "Debug Console\tCtrl+D");
    AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), "&View");

    HMENU tools = CreatePopupMenu();
    AppendMenuA(tools, MF_STRING, ID_TOOL_SELFTEST, "&Self Test");
    AppendMenuA(tools, MF_STRING, ID_TOOL_DIFF, "&Diff Since Snapshot");
    AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), "&Tools");

    HMENU help = CreatePopupMenu();
    AppendMenuA(help, MF_STRING, ID_HELP_ABOUT, "&About");
    AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), "&Help");

    SetMenu(hwnd, bar);
}

void buildAccelerators() {
    ACCEL accels[] = {
        {FVIRTKEY, VK_F5, ID_BUILD_BURNRUN},
        {FVIRTKEY, VK_F7, ID_BUILD_BUILD},
        {FVIRTKEY, VK_F8, ID_DEBUG_STEP},
        {FVIRTKEY, VK_F9, ID_DEBUG_RUN},
        {FCONTROL | FVIRTKEY, 'N', ID_FILE_NEW},
        {FCONTROL | FVIRTKEY, 'O', ID_FILE_OPEN},
        {FCONTROL | FVIRTKEY, 'S', ID_FILE_SAVE},
        {FCONTROL | FSHIFT | FVIRTKEY, 'S', ID_FILE_SAVEAS},
        {FCONTROL | FVIRTKEY, 'R', ID_DEBUG_RESET},
        {FCONTROL | FVIRTKEY, 'T', ID_VIEW_THEME_NEXT},
        {FCONTROL | FVIRTKEY, 'F', ID_EDIT_FIND},
        {FCONTROL | FSHIFT | FVIRTKEY, 'F', ID_EDIT_FINDINFILES},
        {FCONTROL | FVIRTKEY, 'G', ID_EDIT_GOTO},
        {FCONTROL | FSHIFT | FVIRTKEY, 'P', ID_VIEW_PALETTE},
        {FCONTROL | FVIRTKEY, VK_OEM_COMMA, ID_VIEW_SETTINGS},
        {FCONTROL | FVIRTKEY, 'D', ID_VIEW_DEBUGCONSOLE},
        // MRU shortcuts: Ctrl+Shift+1..9 and Ctrl+Shift+0 (item 99).
        {FCONTROL | FSHIFT | FVIRTKEY, '1', ID_RECENT_BASE + 0},
        {FCONTROL | FSHIFT | FVIRTKEY, '2', ID_RECENT_BASE + 1},
        {FCONTROL | FSHIFT | FVIRTKEY, '3', ID_RECENT_BASE + 2},
        {FCONTROL | FSHIFT | FVIRTKEY, '4', ID_RECENT_BASE + 3},
        {FCONTROL | FSHIFT | FVIRTKEY, '5', ID_RECENT_BASE + 4},
        {FCONTROL | FSHIFT | FVIRTKEY, '6', ID_RECENT_BASE + 5},
        {FCONTROL | FSHIFT | FVIRTKEY, '7', ID_RECENT_BASE + 6},
        {FCONTROL | FSHIFT | FVIRTKEY, '8', ID_RECENT_BASE + 7},
        {FCONTROL | FSHIFT | FVIRTKEY, '9', ID_RECENT_BASE + 8},
        {FCONTROL | FSHIFT | FVIRTKEY, '0', ID_RECENT_BASE + 9},
        {FVIRTKEY, VK_F6, ID_DEBUG_QSAVE},
        {FCONTROL | FVIRTKEY, VK_F7, ID_DEBUG_QLOAD},
        {FVIRTKEY, VK_F10, ID_DEBUG_FRAMEADV},
    };
    g_accel = CreateAcceleratorTableA(accels, static_cast<int>(sizeof(accels) / sizeof(accels[0])));
}

void openRecent(int index) {
    const auto &recent = Settings::get().recentFiles;
    if (index < 0 || index >= static_cast<int>(recent.size())) return;
    const std::string path = recent[static_cast<std::size_t>(index)];
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        termError("cannot open " + path + "\r\n");
        return;
    }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!appConfirmDiscard()) return;
    g_app.editor.setText(content);
    g_app.currentFile = path;
    Settings::pushRecent(path);
    setStatusBar("Opened " + path);
}

void handleCommand(WPARAM wParam) {
    const int id = LOWORD(wParam);
    switch (id) {
    case ID_FILE_EXIT:
        PostMessageA(g_app.mainWnd, WM_CLOSE, 0, 0);
        break;
    case ID_FILE_NEW:
        appNewFile();
        break;
    case ID_FILE_OPEN:
        appOpenFileDialog();
        break;
    case ID_FILE_SAVE:
        appSaveFileDialog(false);
        break;
    case ID_FILE_SAVEAS:
        appSaveFileDialog(true);
        break;
    case ID_BUILD_BUILD:
        appBurn(false);
        break;
    case ID_BUILD_BURNRUN:
        appBurn(true);
        break;
    case ID_BUILD_UPLOAD:
        appUpload();
        break;
    case ID_DEBUG_RUN:
        if (g_app.emu.running())
            appPause();
        else
            appStart();
        break;
    case ID_DEBUG_PAUSE:
        appPause();
        break;
    case ID_DEBUG_STEP:
        appStepInstruction();
        break;
    case ID_DEBUG_RESET:
        appReset();
        break;
    case ID_DEBUG_BACK:
        appStepBack();
        break;
    case ID_DEBUG_FORWARD:
        appStepForward();
        break;
    case ID_DEBUG_TOGGLEBP:
        appToggleBreakpointAt(g_app.disasm.selectedAddress());
        break;
    case ID_DEBUG_RUNCURSOR:
        appRunToSelected();
        break;
    case ID_DEBUG_QSAVE:
        execTerminalCommand("state save 0");
        break;
    case ID_DEBUG_QLOAD:
        execTerminalCommand("state load 0");
        break;
    case ID_DEBUG_FRAMEADV: {
        // Frame advance: run exactly one video frame's worth of cycles.
        if (!g_app.emu.running()) {
            std::lock_guard<std::mutex> guard(remote::emulatorMutex());
            const uint64_t target = g_app.emu.cpu().getTotalCycles() + 29780;
            g_app.emu.start();
            while (g_app.emu.running() && !g_app.emu.cpu().halted() &&
                   g_app.emu.cpu().getTotalCycles() < target) {
                if (g_app.dbg.step() <= 0) break;
            }
            g_app.emu.pause();
            g_app.term.append(TermView::Role::Info, "[FRAME] advanced\r\n");
        }
        break;
    }
    case ID_DEBUG_CLEARBP: {
        // Copy addresses first: removeBreakpoint mutates breakpoints().
        std::vector<uint16_t> addresses;
        for (const auto &bp : g_app.dbg.breakpoints()) addresses.push_back(bp.address);
        for (uint16_t addr : addresses) g_app.dbg.removeBreakpoint(addr);
        InvalidateRect(g_app.mainWnd, nullptr, FALSE);
        break;
    }
    case ID_EDIT_UNDO:
        g_app.editor.undo();
        break;
    case ID_EDIT_REDO:
        g_app.editor.redo();
        break;
    case ID_EDIT_CUT:
        g_app.editor.copy(true);
        break;
    case ID_EDIT_COPY:
        g_app.editor.copy(false);
        break;
    case ID_EDIT_PASTE:
        g_app.editor.paste();
        break;
    case ID_EDIT_DUP:
        g_app.editor.duplicateLine();
        break;
    case ID_EDIT_COMMENT:
        g_app.editor.toggleComment();
        break;
    case ID_EDIT_GOTO:
        findBarToggle(g_app.mainWnd, true);
        break;
    case ID_EDIT_FIND:
        findBarToggle(g_app.mainWnd, false);
        break;
    case ID_EDIT_FINDINFILES:
        findInFilesToggle(g_app.mainWnd);
        break;
    case ID_VIEW_THEME_NEXT: {
        AppSettings &s = Settings::get();
        s.themeIndex = (s.themeIndex + 1) % themeCount();
        setThemeIndex(s.themeIndex);
        Settings::save();
        applyThemeToWindow(g_app.mainWnd);
        break;
    }
    case ID_VIEW_SCREENSHOT:
        appScreenshot("");
        break;
    case ID_VIEW_PALETTE:
        paletteToggle(g_app.mainWnd);
        break;
    case ID_VIEW_PALETTE_EDITOR:
        paletteEditorToggle(g_app.mainWnd);
        break;
    case ID_VIEW_SETTINGS:
        settingsDialogShow(g_app.mainWnd);
        break;
    case ID_VIEW_DEBUGCONSOLE:
        g_app.dbgConsole.toggle();
        break;
    case ID_TOOL_SELFTEST:
        runSelfTests();
        break;
    case ID_TOOL_DIFF:
        snapshotDiffViewerShow(g_app.mainWnd);
        break;
    case ID_HELP_ABOUT: {
        MessageBoxA(g_app.mainWnd,
                    "6502 Emulator IDE\n\n"
                    "Tiny chip. Big ideas.\n"
                    "Write, build, run, debug - all in one window.\n",
                    "About", MB_OK);
        break;
    }
    default:
        if (id >= ID_RECENT_BASE && id < ID_RECENT_BASE + 10) openRecent(id - ID_RECENT_BASE);
        break;
    }
    InvalidateRect(g_app.mainWnd, nullptr, FALSE);
}

void tickEmulation() {
    static bool wasRunning = false;
    static uint64_t lastCyclesSnapshot = 0;
    static std::chrono::steady_clock::time_point lastSecond{};
    static int instructionsThisSecond = 0;
    static int framesThisSecond = 0;
    // Item 60: frame pacing. The 15 ms Win32 timer is only a fallback
    // heartbeat; real frame boundaries are scheduled on steady_clock at
    // ~16.67 ms. Beats arriving early sleep 1 ms (timeBeginPeriod(1) is set
    // at startup) instead of busy-burning CPU; late beats run immediately,
    // so the average frame interval stays near 16.67 ms.
    using FrameClock = std::chrono::steady_clock;
    constexpr auto kFrameInterval = std::chrono::microseconds(16667);
    static FrameClock::time_point nextFrame{};
    if (nextFrame.time_since_epoch().count() == 0) nextFrame = FrameClock::now();
    // Pause emulation entirely while the window is minimized (item: perf).
    if (IsIconic(g_app.mainWnd)) return;
    if (FrameClock::now() - nextFrame > std::chrono::milliseconds(120))
        nextFrame = FrameClock::now(); // resync after stall/minimize
    {
        int sleepGuard = 0;
        while (FrameClock::now() < nextFrame - std::chrono::milliseconds(1) && sleepGuard < 16) {
            Sleep(1); // yield instead of spinning when the timer beats early
            ++sleepGuard;
        }
    }
    ++framesThisSecond;

    // OSD toast lifecycle (item 34).
    uiTickToasts();

    // GIF capture start/stop toasts are detected here because the capture
    // commands live in the terminal module; open/close transitions surface
    // through g_app.gif state each tick.
    static bool gifWasOpen = false;
    const bool gifOpen = g_app.gif.isOpen();
    if (gifOpen && !gifWasOpen)
        osdToast("GIF capture started");
    else if (!gifOpen && gifWasOpen)
        osdToast("GIF capture stopped");
    gifWasOpen = gifOpen;

    const bool running = g_app.emu.running();

    if (running) {
        uint64_t budget = std::clamp<uint64_t>(Settings::get().speed, 100, 50000000);
        // Turbo / fast-forward while Tab is held.
        if (Settings::get().turboEnabled && (GetAsyncKeyState(VK_TAB) & 0x8000) != 0) budget *= 8;
        uint64_t executed = 0;
        // Run-to-cursor: the one-shot breakpoint pauses the emulator exactly
        // on the target instruction from inside runInstructions. Reading
        // cpu().PC and dropping the one-shot breakpoint must happen UNDER
        // emulatorMutex() - the remote server can step/inspect the CPU the
        // moment the lock is released.
        bool reachedRunTo = false;
        {
            std::lock_guard<std::mutex> guard(remote::emulatorMutex());
            executed = g_app.dbg.runInstructions(budget);
            if (g_app.runToAddress >= 0 && !g_app.emu.running()) {
                reachedRunTo = g_app.dbg.lastStopWasBreakpoint() &&
                               g_app.emu.cpu().PC == static_cast<uint16_t>(g_app.runToAddress);
                // ANY stop ends a run-to-cursor: drop the one-shot breakpoint here
                // so it cannot leak into the next run when a different breakpoint
                // (or a halt/watchpoint) stopped the emulation first.
                if (g_app.runToBpOurs)
                    g_app.dbg.removeBreakpoint(static_cast<uint16_t>(g_app.runToAddress));
                g_app.runToAddress = -1;
                g_app.runToBpOurs = false;
            }
        }
        instructionsThisSecond += static_cast<int>(executed);
        if (reachedRunTo) {
            g_app.term.append(
                TermView::Role::Warn, "[RUNTO] reached $" + [] {
                    char b[8];
                    std::snprintf(b, sizeof(b), "%04X", g_app.emu.cpu().PC);
                    return std::string(b);
                }() + "\r\n");
            osdToast("Run to cursor");
        } else if (executed == 0) {
            g_app.emu.pause();
            if (g_app.dbg.lastStopWasBreakpoint()) {
                g_app.term.append(TermView::Role::Warn, "[BREAK] breakpoint hit\r\n");
                osdToast("Breakpoint hit");
            } else if (g_app.dbg.lastStopWasWatchpoint()) {
                g_app.term.append(TermView::Role::Warn, "[WATCH] watchpoint hit\r\n");
            } else {
                g_app.term.append(TermView::Role::Err, "[CPU] halted (JAM)\r\n");
            }
        }
    }

    Memory &mem = g_app.emu.memory();
    const AppSettings &keys = Settings::get();
    uint8_t buttons = 0;
    if (g_app.disp.keyDown(keys.vkUp)) buttons |= 0x01;
    if (g_app.disp.keyDown(keys.vkDown)) buttons |= 0x02;
    if (g_app.disp.keyDown(keys.vkLeft)) buttons |= 0x04;
    if (g_app.disp.keyDown(keys.vkRight)) buttons |= 0x08;
    if (g_app.disp.keyDown(keys.vkButtonA)) buttons |= 0x10;
    if (g_app.disp.keyDown(keys.vkButtonB)) buttons |= 0x20;
    if (g_app.disp.keyDown(keys.vkStart)) buttons |= 0x40;
    if (g_app.disp.keyDown(keys.vkSelect)) buttons |= 0x80;
    // Input recording/replay: replay substitutes recorded controller bytes.
    uint8_t effectiveButtons = buttons;
    g_app.input.frameTick(buttons, effectiveButtons);
    // Memory-mapped I/O lives OUTSIDE the default framebuffer window
    // ($0200-$11FF for a 64x64 screen) so the IDE never paints stray
    // pixels into visible video RAM before a program runs.
    mem.writeRaw(0x12E0, effectiveButtons);

    const int mx = g_app.disp.mouseX();
    const int my = g_app.disp.mouseY();
    mem.writeRaw(0x12E1, static_cast<uint8_t>(mx < 0 ? 255 : mx));
    mem.writeRaw(0x12E2, static_cast<uint8_t>(my < 0 ? 255 : my));
    mem.writeRaw(0x12E3,
                 static_cast<uint8_t>((g_app.disp.mouseLeft() ? 1 : 0) | (g_app.disp.mouseRight() ? 2 : 0)));

    static uint8_t lastScan = 0xFF;
    static uint8_t lastVblank = 0xFF;
    const uint64_t cyclesInFrame = g_app.emu.cpu().getTotalCycles() % 29780;
    const uint8_t scan = static_cast<uint8_t>(cyclesInFrame * 255 / 29780);
    const uint8_t vblank = cyclesInFrame >= 27800 ? 1 : 0;
    if (scan != lastScan) {
        mem.writeRaw(0x12F4, scan);
        lastScan = scan;
    }
    if (vblank != lastVblank) {
        mem.writeRaw(0x12F5, vblank);
        lastVblank = vblank;
    }
    mem.writeRaw(0x12F6, g_app.emu.frameCounter());
    g_app.emu.setFrameCounter(static_cast<uint8_t>(g_app.emu.frameCounter() + 1));

    // Debug console output - event-driven (only click events)
    if (g_app.dbgConsole.isVisible()) {
        static bool keysPressed[256] = {false};
        static bool lastMouseLeft = false;
        static bool lastMouseRight = false;

        // Key click events (only on release = "click")
        for (int vk = 1; vk < 256; ++vk) {
            bool down = g_app.disp.keyDown(vk);
            if (!down && keysPressed[vk]) {
                // Key was pressed, now released = click
                char buf[64];
                std::snprintf(buf, sizeof(buf), "[CLICK] VK=%d\r\n", vk);
                g_app.dbgConsole.append(buf);
            }
            keysPressed[vk] = down;
        }

        // Mouse click events
        bool mouseLeft = g_app.disp.mouseLeft();
        bool mouseRight = g_app.disp.mouseRight();

        if (!mouseLeft && lastMouseLeft) {
            char buf[64];
            int mx = g_app.disp.mouseX();
            int my = g_app.disp.mouseY();
            std::snprintf(buf, sizeof(buf), "[MCLICK] L (%d,%d)\r\n", mx, my);
            g_app.dbgConsole.append(buf);
        }
        if (!mouseRight && lastMouseRight) {
            char buf[64];
            int mx = g_app.disp.mouseX();
            int my = g_app.disp.mouseY();
            std::snprintf(buf, sizeof(buf), "[MCLICK] R (%d,%d)\r\n", mx, my);
            g_app.dbgConsole.append(buf);
        }
        lastMouseLeft = mouseLeft;
        lastMouseRight = mouseRight;
    }

    const AppSettings &cfg = Settings::get();
    g_app.disp.renderMemory(mem.data(), cfg.fbBase, cfg.fbSize, cfg.fbSize);

    // Animated GIF capture: append the freshly rendered frame.
    if (g_app.gif.isOpen()) {
        static unsigned gifFramesLogged = 0;
        std::string err;
        if (!g_app.disp.frame().empty()) {
            if (!g_app.gif.addFrame(g_app.disp.frame().data(), cfg.fbSize, cfg.fbSize, err)) {
                g_app.term.append(TermView::Role::Err, "[GIF] " + err + "\r\n");
                g_app.gif.close(err);
            } else if (++gifFramesLogged >= 10000) {
                g_app.gif.close(err);
                g_app.term.append(TermView::Role::Info, "[GIF] capture finished\r\n");
            }
        }
    }

    g_app.hex.refresh();
    g_app.disasm.refresh(false);
    g_app.regs.refresh();
    // Only invalidate status bar (IPS/PC/running state) — children self-invalidate
    RECT statusRc = uiStatusRect();
    InvalidateRect(g_app.mainWnd, &statusRc, FALSE);

    const auto nowSecond = std::chrono::steady_clock::now();
    if (lastSecond.time_since_epoch().count() == 0) lastSecond = nowSecond;
    if (nowSecond - lastSecond > std::chrono::seconds(1)) {
        const double secs = std::chrono::duration<double>(nowSecond - lastSecond).count();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d ips",
                      static_cast<int>(instructionsThisSecond / std::max(1e-6, secs)));
        setIpsText(buf);
        // Item 33: expose FPS / MIPS / total cycles to the status bar.
        uiSetPerf(instructionsThisSecond / std::max(1e-6, secs) / 1e6,
                  static_cast<int>(framesThisSecond / std::max(1e-6, secs) + 0.5),
                  g_app.emu.cpu().getTotalCycles());
        instructionsThisSecond = 0;
        framesThisSecond = 0;
        lastSecond = nowSecond;
    }
    nextFrame += kFrameInterval;
    (void)lastCyclesSnapshot;
    (void)wasRunning;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_app.mainWnd = hwnd;
        rebuildFontsAndInvalidate();

        // Item 8c: editor symbol hover lookup from the assembled program.
        g_app.editor.setSymbolLookup([](const std::string &name, uint16_t &value) {
            if (!g_app.emu.hasProgram()) return false;
            const auto &symbols = g_app.emu.program().symbols;
            auto it = symbols.find(name);
            if (it == symbols.end()) {
                std::string upper;
                upper.reserve(name.size());
                for (char c : name)
                    upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                it = symbols.find(upper);
                if (it == symbols.end()) return false;
            }
            value = it->second;
            return true;
        });

        DragAcceptFiles(hwnd, TRUE);
        timeBeginPeriod(1); // fine-grained Sleep(1) for frame pacing

        g_app.cmdInput = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                         0, 0, 200, 26, hwnd, nullptr, GetModuleHandleA(nullptr), nullptr);
        SendMessageA(g_app.cmdInput, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.mono), TRUE);
        g_oldInputProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(g_app.cmdInput, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(inputProc)));

        g_app.editor.create(hwnd, g_app.mono, 1001);
        g_app.hex.create(hwnd, g_app.mono);
        g_app.disasm.create(hwnd, g_app.mono);
        g_app.regs.create(hwnd, g_app.mono, g_app.monoBold);
        g_app.term.create(hwnd, g_app.mono);
        g_app.dbgConsole.create(hwnd, g_app.mono);
        g_app.hex.attach(&g_app.emu.memory());
        g_app.disasm.attach(&g_app.emu, &g_app.dbg);
        g_app.regs.attach(&g_app.emu, &g_app.dbg);
        g_app.disp.createChild(hwnd, Settings::get().fbSize, Settings::get().fbSize);
        g_app.disp.setSmoothScaling(Settings::get().smoothScreen);

        g_app.editor.setText(defaultEditorSource());
        g_app.term.append(TermView::Role::Info,
                          "6502 IDE ready. Type 'help' below. Ctrl+Shift+P for command palette.\r\n");
        buildMenu(hwnd);
        buildAccelerators();
        uiApplyLayout();
        if (Settings::get().remotePort > 0) {
            std::string err;
            RemoteServer::instance().start(g_app.emu, g_app.dbg, Settings::get().remotePort, err);
            g_app.term.append(TermView::Role::Info, err + "\r\n");
        }
        SetTimer(hwnd, TIMER_ID, TIMER_MS, nullptr);
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        winutil::fillRectColor(dc, rc, theme().windowBg);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right - rc.left, rc.bottom - rc.top);
        HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(mem, bmp));
        uiPaintToolbar(mem, uiToolbarRect());
        uiPaintStatus(mem, uiStatusRect());
        uiPaintToasts(mem, rc); // OSD toast overlay (item 34)
        const Theme &t = theme();
        for (int i = 1; i <= 5; ++i) winutil::fillRectColor(mem, uiSplitterRect(i), t.windowBg);
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        const int btnIndex = uiToolbarButtonAt(x, y);
        if (btnIndex >= 0) {
            uiSetPressedButton(btnIndex);
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        const int splitter = uiSplitterHitTest(x, y);
        if (splitter) {
            uiBeginDrag(splitter, x, y);
            SetCapture(hwnd);
            return 0;
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        const bool captured = GetCapture() == hwnd;
        if (!captured) {
            const int hover = uiToolbarButtonAt(x, y);
            static int lastHover = -1;
            if (hover != lastHover) {
                lastHover = hover;
                uiSetHoverButton(hover);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        if (uiPressedButton() >= 0) return 0;
        if (uiDragging()) uiDragTo(x, y);
        return 0;
    }
    case WM_MOUSELEAVE:
        uiSetHoverButton(-1);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        const bool wasPressing = uiPressedButton() >= 0;
        const int pressedNow = uiPressedButton();
        uiSetPressedButton(-1);
        ReleaseCapture();
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        if (wasPressing && uiToolbarButtonAt(x, y) == pressedNow)
            PostMessageA(hwnd, WM_COMMAND, uiButtonCommandId(pressedNow), 0);
        uiEndDrag();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_SETCURSOR: {
        DWORD pos = GetMessagePos();
        const int x = GET_X_LPARAM(pos);
        const int y = GET_Y_LPARAM(pos);
        const int splitter = uiSplitterHitTest(x, y);
        if (splitter) {
            // 3/4/5 are horizontal splits (screen|term, regs|hex, hex|disasm).
            SetCursor(LoadCursorA(nullptr, splitter >= 3 ? IDC_SIZENS : IDC_SIZEWE));
            return TRUE;
        }
        break;
    }
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            uiComputeLayout(LOWORD(lParam), HIWORD(lParam));
            uiApplyLayout();
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    case WM_COMMAND:
        handleCommand(wParam);
        return 0;
    case WM_APP + 40:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_APP + 42:
        if (wParam == 1) appToggleBreakpointAt(static_cast<uint16_t>(lParam));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_APP + 43:
        appPause();
        return 0;
    case WM_APP + 45:
        if (wParam == 1) {
            const auto *textPtr = reinterpret_cast<const std::string *>(lParam);
            if (textPtr) {
                g_app.editor.findNext(*textPtr, true, false);
                delete textPtr;
            }
        } else if (wParam == 2) {
            g_app.editor.goToLine(static_cast<int>(lParam) - 1);
        }
        return 0;
    case WM_DROPFILES: {
        // Item 29: open the first dropped file through the same path as
        // ID_FILE_OPEN.
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        char path[MAX_PATH]{};
        if (DragQueryFileA(drop, 0, path, MAX_PATH) > 0) appOpenFileAtPath(path);
        DragFinish(drop);
        return 0;
    }
    case WM_EDITOR_ZOOM: {
        // Item 8a: Ctrl+wheel zoom from the editor.
        AppSettings &s = Settings::get();
        s.editorFontSize = std::clamp(s.editorFontSize + (wParam > 0 ? 1 : -1), 8, 32);
        rebuildFontsAndInvalidate();
        Settings::save();
        return 0;
    }
    case WM_TIMER:
        if (wParam == TIMER_ID) tickEmulation();
        return 0;
    case WM_CLOSE: {
        AppSettings &s = Settings::get();
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        GetWindowPlacement(hwnd, &wp);
        s.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
        Settings::save();
        if (!appConfirmDiscard()) return 0;
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        timeEndPeriod(1);
        // RemoteServer::stop() shuts the client socket down before joining the
        // worker, so it is safe to call here even while a client is connected.
        RemoteServer::instance().stop();
        if (g_accel) DestroyAcceleratorTable(g_accel);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// Exact-token match for a flag in the raw command line (quote-aware, the same
// split rules as headless.cpp's splitArgs) so a file name that merely CONTAINS
// "--headless" does not dispatch the GUI into headless mode.
bool cmdLineHasFlag(const char *cmdLine, const char *flag) {
    if (!cmdLine) return false;
    std::string token;
    bool quote = false;
    for (const char *p = cmdLine;; ++p) {
        const char c = *p;
        const bool end = c == '\0';
        if (c == '"')
            quote = !quote;
        else if (end || (!quote && (c == ' ' || c == '\t'))) {
            if (token == flag) return true;
            token.clear();
            if (end) return false;
        } else
            token += c;
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR cmdLine, int) {
    installCrashHandler();
    // Load INI settings BEFORE dispatching headless: the CLI honours the same
    // 6502_ide.ini (include paths, warn level, defines). Settings::load() has
    // no window/GUI dependency (settings.cpp:148 only reads files/env).
    Settings::load();
    if (cmdLineHasFlag(cmdLine, "--headless")) {
        return headlessMain(cmdLine);
    }

    setThemeIndex(Settings::get().themeIndex);

    WNDCLASSA wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = APP_CLASS;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    const UINT exStyle = WS_EX_APPWINDOW;
    HWND hwnd =
        CreateWindowExA(exStyle, APP_CLASS, "6502 Emulator IDE", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                        CW_USEDEFAULT, CW_USEDEFAULT, Settings::get().windowW, Settings::get().windowH,
                        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, Settings::get().maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        // While a modal sheet (settings dialog) is open, route keys through
        // IsDialogMessage for Tab/Enter/Esc navigation and skip accelerators.
        const HWND sheet = uiModalSheetWindow();
        if (sheet) {
            if (IsDialogMessageA(sheet, &msg)) continue;
        } else if (g_accel && TranslateAcceleratorA(g_app.mainWnd, g_accel, &msg)) {
            // Always target the main window: accelerators pressed while a
            // child (editor/terminal/palette edit box) has focus would
            // otherwise dispatch WM_COMMAND to that child and vanish.
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    Settings::save();
    return static_cast<int>(msg.wParam);
}
