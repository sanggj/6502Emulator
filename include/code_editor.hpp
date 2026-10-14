#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
inline constexpr UINT WM_EDITOR_ZOOM = WM_APP + 0x22;
#endif

class CodeEditor {
  public:
    struct ErrorMark {
        int line = 0;
        int column = 0;
        std::string message;
    };

    bool create(HWND parent, HFONT font, int id);
    void setFont(HFONT font);
    HWND hwnd() const noexcept { return hwnd_; }

    void setText(const std::string &text);
    std::string getText() const;
    bool modified() const noexcept { return modified_; }
    void clearModified() noexcept { modified_ = false; }

    void setSymbolLookup(std::function<bool(const std::string &name, uint16_t &value)> lookup);
    void setErrorMarks(std::vector<ErrorMark> marks);

    int caretLine() const noexcept { return caret_.line; }
    int caretColumn() const noexcept { return caret_.col; }
    void goToLine(int line);
    int findNext(const std::string &needle, bool down, bool caseSensitive);

    void undo();
    void redo();
    bool canUndo() const noexcept { return !undoStack_.empty(); }
    bool canRedo() const noexcept { return !redoStack_.empty(); }
    void selectAll();
    void copy(bool cut);
    void paste();
    void toggleComment();
    void duplicateLine();
    void moveLines(int delta);
    void deleteSelection();

  private:
    struct Caret {
        int line = 0;
        int col = 0;
        bool operator==(const Caret &o) const { return line == o.line && col == o.col; }
    };

    void measureFont();
    void updateScrollbars();
    void ensureCaretVisible();
    void clampCaret();
    void notifyParent(int code) const;

    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void onPaint(HDC dc, const RECT &rcPaint);
    void paintBuffer(HDC buffer, const RECT &full);
    void drawGutter(HDC dc, int width);
    void drawLineText(HDC dc, int lineIndex, int screenY, int textOffsetX);
    void tokenize(int lineIndex, std::vector<std::pair<int, COLORREF>> &colors) const;

    HWND ensurePopup(bool autocomplete);
    static void registerPopupClass();
    static LRESULT CALLBACK popupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void placeAutocompletePopup();
    void refreshAutocompletePos();
    void paintAutocompletePopup(HDC dc, const RECT &rc);
    void paintHoverTipPopup(HDC dc, const RECT &rc);
    void updateAutocomplete();
    void hideAutocomplete();
    void acceptAutocomplete();
    void autoCompleteClickedAt(int x, int y);
    void updateHoverTip(int x, int y);
    void hideHoverTip();

    std::pair<const ErrorMark *, const ErrorMark *> marksOnLine(int line) const;
    void drawErrorMarksRow(HDC dc, int lineIndex, int screenY, int textOffsetX) const;
    static void drawSquiggle(HDC dc, int x0, int x1, int y, COLORREF color);

    void insertTextAt(const std::string &text, bool selectAfter);
    void replaceSelection(const std::string &text);
    std::string selectedText() const;
    bool hasSelection() const noexcept { return !(anchor_ == caret_); }
    void pushUndo();
    void applyUndo(std::vector<std::vector<std::string>> &from, std::vector<std::vector<std::string>> &to);

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleKey(UINT msg, WPARAM wParam, LPARAM lParam);
    CodeEditor *self() { return this; }

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    int charW_ = 8;
    int lineH_ = 18;
    int gutterW_ = 48;

    std::vector<std::string> lines_{""};
    Caret caret_{};
    Caret anchor_{};
    int topLine_ = 0;
    int leftCol_ = 0;
    int pageRows_ = 20;
    bool modified_ = false;
    bool dragging_ = false;
    bool inHorizDrag_ = false;

    std::vector<std::vector<std::string>> undoStack_;
    std::vector<std::vector<std::string>> redoStack_;

    HWND acPopup_ = nullptr;
    HWND tipPopup_ = nullptr;
    std::vector<std::string> acItems_;
    int acSel_ = 0;
    int acAnchorLine_ = 0;
    int acAnchorCol_ = 0;

    std::function<bool(const std::string &, uint16_t &)> symbolLookup_;
    std::vector<ErrorMark> errorMarks_;

    std::string tipWord_;
    std::string tipText_;
    uint16_t tipValue_ = 0;
    bool tipFound_ = false;
    bool trackingMouseLeave_ = false;
};
