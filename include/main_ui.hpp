#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>
#include <string>

enum CommandId {
    ID_FILE_NEW = 2001,
    ID_FILE_OPEN,
    ID_FILE_SAVE,
    ID_FILE_SAVEAS,
    ID_RECENT_BASE = 2100,
    ID_BUILD_BUILD = 2201,
    ID_BUILD_BURNRUN,
    ID_BUILD_UPLOAD,
    ID_DEBUG_RUN = 2301,
    ID_DEBUG_PAUSE,
    ID_DEBUG_STEP,
    ID_DEBUG_RESET,
    ID_DEBUG_BACK,
    ID_DEBUG_FORWARD,
    ID_DEBUG_TOGGLEBP,
    ID_DEBUG_RUNCURSOR,
    ID_DEBUG_CLEARBP,
    ID_DEBUG_FRAMEADV,
    ID_EDIT_UNDO = 2401,
    ID_EDIT_REDO,
    ID_EDIT_CUT,
    ID_EDIT_COPY,
    ID_EDIT_PASTE,
    ID_EDIT_DUP,
    ID_EDIT_COMMENT,
    ID_EDIT_GOTO,
    ID_EDIT_FIND,
    ID_EDIT_FINDINFILES,
    ID_VIEW_THEME_NEXT = 2501,
    ID_VIEW_SCREENSHOT,
    ID_VIEW_PALETTE,
    ID_VIEW_PALETTE_EDITOR,
    ID_VIEW_SETTINGS,
    ID_HELP_ABOUT = 2601,
    ID_TOOL_SELFTEST = 2701,
    ID_TOOL_DIFF,
};

void uiPaintToolbar(HDC dc, const RECT &rc);
void uiPaintStatus(HDC dc, const RECT &rc);
RECT uiToolbarRect();
RECT uiStatusRect();
RECT uiSplitterRect(int which);
int uiToolbarButtonAt(int x, int y);
void uiSetPressedButton(int index);
int uiPressedButton() noexcept;
int uiHoverButton() noexcept;
void uiSetHoverButton(int index);
int uiButtonCommandId(int index);

void uiComputeLayout(int w, int h);
void uiApplyLayout();
int uiSplitterHitTest(int x, int y);
void uiBeginDrag(int mode, int startX, int startY);
void uiDragTo(int x, int y);
bool uiDragging();
void uiEndDrag();

void rebuildFontsAndInvalidate();
void refreshRecentMenu(HMENU menu);

void paletteToggle(HWND owner);
bool paletteIsVisible();

void findBarToggle(HWND owner, bool gotoMode);
bool findBarVisible();
const std::string &findBarText();

// OSD toast notification (bottom-right overlay, auto-expires ~1.8 s).
void osdToast(const std::string &text);
void uiTickToasts();
void uiPaintToasts(HDC dc, const RECT &client);

// Live performance counters for the status bar (item 33).
void uiSetPerf(double mips, int fps, uint64_t totalCycles);

void findInFilesToggle(HWND owner);
void settingsDialogShow(HWND owner);
HWND uiModalSheetWindow();
void snapshotDiffViewerShow(HWND owner);
void paletteEditorToggle(HWND owner);
bool paletteEditorIsVisible();

void setIpsText(const std::string &text);
