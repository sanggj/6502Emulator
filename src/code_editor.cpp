#include "code_editor.hpp"
#include "assembler.hpp"
#include "theme.hpp"
#include "win_util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) static_cast<int>(static_cast<short>(LOWORD(lp)))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) static_cast<int>(static_cast<short>(HIWORD(lp)))
#endif
#ifndef GET_WHEEL_DELTA_WPARAM
#define GET_WHEEL_DELTA_WPARAM(wParam) ((short)HIWORD(wParam))
#endif

namespace {

std::string trimLocal(const std::string &s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t") - first + 1);
}

std::string upperAscii(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

const std::unordered_set<std::string> &mnemonicSet() {
    static const std::unordered_set<std::string> set = {
        "ADC", "AND", "ASL", "BCC", "BCS", "BEQ", "BIT", "BMI", "BNE", "BPL", "BRK", "BVC", "BVS",
        "CLC", "CLD", "CLI", "CLV", "CMP", "CPX", "CPY", "DEC", "DEX", "DEY", "EOR", "INC", "INX",
        "INY", "JMP", "JSR", "LDA", "LDX", "LDY", "LSR", "NOP", "ORA", "PHA", "PHP", "PLA", "PLP",
        "ROL", "ROR", "RTI", "RTS", "SBC", "SEC", "SED", "SEI", "STA", "STX", "STY", "TAX", "TAY",
        "TSX", "TXA", "TXS", "TYA", "SLO", "RLA", "SRE", "RRA", "SAX", "LAX", "DCP", "ISC", "ANC",
        "ALR", "ARR", "SBX", "XAA", "SHY", "SHX", "SHA", "TAS", "LAS", "JAM"};
    return set;
}

const std::unordered_set<std::string> &directiveSet() {
    static const std::unordered_set<std::string> set = {
        ".ORG",    ".EQU",     ".DEFINE", ".BYTE",  ".DB",      ".WORD",   ".DW",   ".ASCII",
        ".ASCIZ",  ".RES",     ".FILL",   ".END",   ".ALIGN",   ".REPEAT", ".ENDR", ".MACRO",
        ".ENDM",   ".INCLUDE", ".IF",     ".IFDEF", ".IFNDEF",  ".ELSE",   ".ELIF", ".ENDIF",
        ".ASSERT", ".ENTRY",   ".ERROR",  ".WARN",  ".SEGMENT", ".HEX"};
    return set;
}

bool isWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '@';
}

const char *const kCompletionDirectives[] = {
    ".org",    ".equ",     ".define", ".byte",  ".db",      ".word",   ".dw",    ".ascii",
    ".asciz",  ".hex",     ".res",    ".fill",  ".align",   ".repeat", ".endr",  ".macro",
    ".endm",   ".include", ".if",     ".ifdef", ".ifndef",  ".else",   ".elif",  ".endif",
    ".assert", ".entry",   ".error",  ".warn",  ".segment", ".end",    ".break", ".xref"};

const std::vector<std::string> &completionList() {
    static const std::vector<std::string> list = [] {
        std::unordered_set<std::string> seen;
        std::vector<std::string> mnemonics;
        const Assembler::OpcodeInfo *table = Assembler::opcodeTable();
        const std::size_t count = Assembler::opcodeTableSize();
        for (std::size_t i = 0; i < count; ++i) {
            const std::string name = table[i].name;
            if (seen.insert(name).second) mnemonics.push_back(name);
        }
        std::sort(mnemonics.begin(), mnemonics.end());
        std::vector<std::string> directives;
        for (const char *d : kCompletionDirectives) directives.push_back(d);
        std::sort(directives.begin(), directives.end());
        mnemonics.insert(mnemonics.end(), directives.begin(), directives.end());
        return mnemonics;
    }();
    return list;
}

bool ciStartsWith(const std::string &item, const std::string &upperPrefix) {
    if (item.size() < upperPrefix.size()) return false;
    for (std::size_t i = 0; i < upperPrefix.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(item[i])) != upperPrefix[i]) return false;
    return true;
}

struct EditorPopupTag {
    CodeEditor *ed;
    bool autocomplete;
};

std::vector<std::string> splitLines(const std::string &text) {
    std::vector<std::string> out;
    std::string current;
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') {
            out.push_back(current);
            current.clear();
        } else
            current += c;
    }
    out.push_back(current);
    return out;
}

} // namespace

bool CodeEditor::create(HWND parent, HFONT font, int id) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = &CodeEditor::proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "6502CodeEditor";
    wc.hCursor = LoadCursorA(nullptr, IDC_IBEAM);
    RegisterClassA(&wc);
    font_ = font;
    hwnd_ = CreateWindowExA(
        0, "6502CodeEditor", nullptr, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_CLIPSIBLINGS, 0, 0,
        400, 300, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleA(nullptr), this);
    if (!hwnd_) return false;
    measureFont();
    return true;
}

void CodeEditor::setFont(HFONT font) {
    font_ = font;
    measureFont();
}

LRESULT CALLBACK CodeEditor::proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTA *>(lParam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        // make hwnd_ valid before any message handler runs: handlers pass hwnd_
        // to DefWindowProcA / InvalidateRect / BeginPaint etc.
        reinterpret_cast<CodeEditor *>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto *self = reinterpret_cast<CodeEditor *>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcA(hwnd, msg, wParam, lParam);
    if (!self->hwnd_) self->hwnd_ = hwnd;
    return self->handleMessage(msg, wParam, lParam);
}

void CodeEditor::measureFont() {
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
    int digits = 3;
    for (int n = static_cast<int>(lines_.size()); n >= 1000; n /= 10) ++digits;
    gutterW_ = digits * charW_ + 14;
    updateScrollbars();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void CodeEditor::notifyParent(int code) const {
    if (!hwnd_) return;
    SendMessageA(GetParent(hwnd_), WM_APP + 40, static_cast<WPARAM>(code), reinterpret_cast<LPARAM>(hwnd_));
}

void CodeEditor::updateScrollbars() {
    RECT rc{};
    if (!hwnd_ || !GetClientRect(hwnd_, &rc)) return;
    pageRows_ = std::max(1, static_cast<int>(rc.bottom - rc.top) / std::max(1, lineH_));
    const int longest = [&] {
        int len = 20;
        for (const auto &l : lines_) len = std::max(len, static_cast<int>(l.size()));
        return len;
    }();
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = static_cast<int>(lines_.size()) + pageRows_;
    si.nPage = static_cast<UINT>(pageRows_);
    si.nPos = topLine_;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);

    const int visibleCols =
        std::max(1, (static_cast<int>(rc.right - rc.left) - gutterW_) / std::max(1, charW_));
    SCROLLINFO sh{};
    sh.cbSize = sizeof(sh);
    sh.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    sh.nMin = 0;
    sh.nMax = longest + visibleCols;
    sh.nPage = static_cast<UINT>(visibleCols);
    sh.nPos = leftCol_;
    SetScrollInfo(hwnd_, SB_HORZ, &sh, TRUE);
}

void CodeEditor::clampCaret() {
    caret_.line = std::clamp(caret_.line, 0, static_cast<int>(lines_.size()) - 1);
    caret_.col =
        std::clamp(caret_.col, 0, static_cast<int>(lines_[static_cast<std::size_t>(caret_.line)].size()));
}

void CodeEditor::ensureCaretVisible() {
    clampCaret();
    if (caret_.line < topLine_) topLine_ = caret_.line;
    if (caret_.line >= topLine_ + pageRows_) topLine_ = caret_.line - pageRows_ + 1;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int visibleCols =
        std::max(1, (static_cast<int>(rc.right - rc.left) - gutterW_) / std::max(1, charW_));
    if (caret_.col < leftCol_) leftCol_ = std::max(0, caret_.col - visibleCols / 4);
    if (caret_.col >= leftCol_ + visibleCols) leftCol_ = caret_.col - visibleCols + 1;
    if (leftCol_ < 0) leftCol_ = 0;
    updateScrollbars();
}

void CodeEditor::pushUndo() {
    undoStack_.push_back(lines_);
    if (undoStack_.size() > 200) undoStack_.erase(undoStack_.begin());
    redoStack_.clear();
    errorMarks_.clear();
    hideHoverTip();
}

void CodeEditor::applyUndo(std::vector<std::vector<std::string>> &from,
                           std::vector<std::vector<std::string>> &to) {
    if (from.empty()) return;
    to.push_back(lines_);
    lines_ = from.back();
    from.pop_back();
    modified_ = true;
    errorMarks_.clear();
    hideHoverTip();
    hideAutocomplete();
    clampCaret();
    ensureCaretVisible();
    InvalidateRect(hwnd_, nullptr, FALSE);
    notifyParent(1);
}

void CodeEditor::undo() {
    applyUndo(undoStack_, redoStack_);
}
void CodeEditor::redo() {
    applyUndo(redoStack_, undoStack_);
}

void CodeEditor::selectAll() {
    anchor_ = {0, 0};
    caret_ = {static_cast<int>(lines_.size()) - 1, static_cast<int>(lines_.back().size())};
    ensureCaretVisible();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::string CodeEditor::selectedText() const {
    if (!hasSelection()) return {};
    Caret a = anchor_;
    Caret b = caret_;
    if (a.line > b.line || (a.line == b.line && a.col > b.col)) std::swap(a, b);
    if (a.line == b.line)
        return lines_[static_cast<std::size_t>(a.line)].substr(static_cast<std::size_t>(a.col),
                                                               static_cast<std::size_t>(b.col - a.col));
    std::string out =
        lines_[static_cast<std::size_t>(a.line)].substr(static_cast<std::size_t>(a.col)) + "\r\n";
    for (int ln = a.line + 1; ln < b.line; ++ln) out += lines_[static_cast<std::size_t>(ln)] + "\r\n";
    out += lines_[static_cast<std::size_t>(b.line)].substr(0, static_cast<std::size_t>(b.col));
    return out;
}

void CodeEditor::deleteSelection() {
    if (!hasSelection()) return;
    Caret a = anchor_;
    Caret b = caret_;
    if (a.line > b.line || (a.line == b.line && a.col > b.col)) std::swap(a, b);
    if (a.line == b.line) {
        auto &l = lines_[static_cast<std::size_t>(a.line)];
        l.erase(l.begin() + a.col, l.begin() + b.col);
    } else {
        auto &first = lines_[static_cast<std::size_t>(a.line)];
        auto &last = lines_[static_cast<std::size_t>(b.line)];
        first.erase(first.begin() + a.col, first.end());
        last.erase(last.begin(), last.begin() + b.col);
        first += last;
        lines_.erase(lines_.begin() + a.line + 1, lines_.begin() + b.line + 1);
    }
    caret_ = a;
    anchor_ = a;
    clampCaret();
}

void CodeEditor::replaceSelection(const std::string &text) {
    pushUndo();
    hideAutocomplete();
    deleteSelection();
    insertTextAt(text, false);
    modified_ = true;
    ensureCaretVisible();
    InvalidateRect(hwnd_, nullptr, FALSE);
    notifyParent(1);
}

void CodeEditor::insertTextAt(const std::string &text, bool) {
    auto parts = splitLines(text);
    auto &line = lines_[static_cast<std::size_t>(caret_.line)];
    std::string tail = line.substr(static_cast<std::size_t>(caret_.col));
    line = line.substr(0, static_cast<std::size_t>(caret_.col)) + parts.front();
    caret_.col = static_cast<int>(line.size());
    for (std::size_t i = 1; i < parts.size(); ++i) {
        lines_.insert(lines_.begin() + caret_.line + static_cast<int>(i), parts[i]);
        caret_.line += 1;
        caret_.col = static_cast<int>(parts[i].size());
    }
    lines_[static_cast<std::size_t>(caret_.line)] += tail;
}

void CodeEditor::copy(bool cut) {
    const std::string sel = selectedText();
    if (sel.empty()) return;
    winutil::clipboardSet(hwnd_, sel);
    if (cut) {
        pushUndo();
        deleteSelection();
        modified_ = true;
        ensureCaretVisible();
        InvalidateRect(hwnd_, nullptr, FALSE);
        notifyParent(1);
    }
}

void CodeEditor::paste() {
    const std::string clip = winutil::clipboardGet(hwnd_);
    if (clip.empty()) return;
    replaceSelection(clip);
}

void CodeEditor::toggleComment() {
    pushUndo();
    Caret a = anchor_;
    Caret b = caret_;
    if (a.line > b.line) std::swap(a, b);
    for (int ln = a.line; ln <= b.line; ++ln) {
        auto &l = lines_[static_cast<std::size_t>(ln)];
        const auto firstNonWs = l.find_first_not_of(" \t");
        if (firstNonWs != std::string::npos && l.compare(firstNonWs, 1, ";") == 0) {
            std::size_t removeLen = 1;
            if (l.size() > firstNonWs + 1 && l[firstNonWs + 1] == ' ') removeLen = 2;
            l.erase(firstNonWs, removeLen);
        } else if (!trimLocal(l).empty()) {
            l.insert(firstNonWs == std::string::npos ? 0 : firstNonWs, "; ");
        }
    }
    anchor_ = a;
    modified_ = true;
    ensureCaretVisible();
    InvalidateRect(hwnd_, nullptr, FALSE);
    notifyParent(1);
}

void CodeEditor::duplicateLine() {
    pushUndo();
    const std::string copyOf = lines_[static_cast<std::size_t>(caret_.line)];
    lines_.insert(lines_.begin() + caret_.line + 1, copyOf);
    caret_.line += 1;
    anchor_ = caret_;
    modified_ = true;
    ensureCaretVisible();
    InvalidateRect(hwnd_, nullptr, FALSE);
    notifyParent(1);
}

void CodeEditor::moveLines(int delta) {
    const int count = static_cast<int>(lines_.size());
    Caret a = anchor_;
    Caret b = caret_;
    if (a.line > b.line) std::swap(a, b);
    const int first = a.line;
    const int last = b.line;
    if (delta < 0 && first == 0) return;
    if (delta > 0 && last == count - 1) return;
    pushUndo();
    std::vector<std::string> block;
    for (int ln = first; ln <= last; ++ln) block.push_back(lines_[static_cast<std::size_t>(ln)]);
    lines_.erase(lines_.begin() + first, lines_.begin() + last + 1);
    const int newInsertAt = delta < 0 ? first - 1 : last + 1 - block.size() + 1;
    lines_.insert(lines_.begin() + newInsertAt, block.begin(), block.end());
    anchor_ = {newInsertAt, a.col};
    caret_ = {newInsertAt + (last - first), b.col};
    modified_ = true;
    ensureCaretVisible();
    InvalidateRect(hwnd_, nullptr, FALSE);
    notifyParent(1);
}

void CodeEditor::setText(const std::string &text) {
    lines_ = splitLines(text);
    caret_ = {0, 0};
    anchor_ = caret_;
    topLine_ = 0;
    leftCol_ = 0;
    errorMarks_.clear();
    hideHoverTip();
    hideAutocomplete();
    clearModified();
    undoStack_.clear();
    redoStack_.clear();
    measureFont();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::string CodeEditor::getText() const {
    std::string out;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        if (i) out += "\r\n";
        out += lines_[i];
    }
    return out;
}

void CodeEditor::goToLine(int line) {
    caret_ = {std::clamp(line, 0, static_cast<int>(lines_.size()) - 1), 0};
    anchor_ = caret_;
    ensureCaretVisible();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int CodeEditor::findNext(const std::string &needle, bool down, bool caseSensitive) {
    if (needle.empty()) return -2;
    const int count = static_cast<int>(lines_.size());
    auto matchIn = [&](const std::string &hay) {
        if (caseSensitive) return hay.find(needle) != std::string::npos;
        return upperAscii(hay).find(upperAscii(needle)) != std::string::npos;
    };
    for (int step = 0; step <= count; ++step) {
        const int ln = down ? (caret_.line + step) % count : ((caret_.line - step) % count + count) % count;
        const auto &lineText = lines_[static_cast<std::size_t>(ln)];
        if (matchIn(lineText)) {
            caret_ = {ln, 0};
            anchor_ = {ln, static_cast<int>(lineText.size())};
            ensureCaretVisible();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return step == 0 ? 0 : 1;
        }
    }
    return -2;
}

void CodeEditor::setSymbolLookup(std::function<bool(const std::string &, uint16_t &)> lookup) {
    symbolLookup_ = std::move(lookup);
    hideHoverTip();
}

void CodeEditor::setErrorMarks(std::vector<ErrorMark> marks) {
    std::sort(marks.begin(), marks.end(), [](const ErrorMark &a, const ErrorMark &b) {
        if (a.line != b.line) return a.line < b.line;
        return a.column < b.column;
    });
    errorMarks_ = std::move(marks);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void CodeEditor::registerPopupClass() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSA wc{};
    wc.lpfnWndProc = &CodeEditor::popupProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "6502EditorPopup";
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
}

HWND CodeEditor::ensurePopup(bool autocomplete) {
    HWND &slot = autocomplete ? acPopup_ : tipPopup_;
    if (slot && IsWindow(slot)) return slot;
    registerPopupClass();
    auto *tag = new EditorPopupTag{this, autocomplete};
    slot = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "6502EditorPopup", nullptr, WS_POPUP, 0, 0,
                           10, 10, hwnd_, nullptr, GetModuleHandleA(nullptr), tag);
    if (!slot) {
        delete tag;
        return nullptr;
    }
    return slot;
}

LRESULT CALLBACK CodeEditor::popupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTA *>(lParam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    auto *tag = reinterpret_cast<EditorPopupTag *>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (!tag || !tag->ed) return DefWindowProcA(hwnd, msg, wParam, lParam);
    CodeEditor &ed = *tag->ed;
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (tag->autocomplete)
            ed.paintAutocompletePopup(dc, ps.rcPaint);
        else
            ed.paintHoverTipPopup(dc, ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN:
        if (tag->autocomplete)
            ed.autoCompleteClickedAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        else
            ed.hideHoverTip();
        return 0;
    case WM_DESTROY:
        if (tag->autocomplete)
            ed.acPopup_ = nullptr;
        else
            ed.tipPopup_ = nullptr;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        delete tag;
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void CodeEditor::placeAutocompletePopup() {
    if (acItems_.empty()) return;
    HWND popup = ensurePopup(true);
    if (!popup) return;
    int width = 120;
    HDC dc = GetDC(hwnd_);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, font_));
    SIZE sz{};
    for (const std::string &item : acItems_) {
        GetTextExtentPoint32A(dc, item.c_str(), static_cast<int>(item.size()), &sz);
        width = std::max(width, static_cast<int>(sz.cx) + 24);
    }
    SelectObject(dc, oldFont);
    ReleaseDC(hwnd_, dc);
    const int rows = static_cast<int>(std::min<std::size_t>(acItems_.size(), 8));
    const int height = rows * lineH_ + 8;

    POINT origin{gutterW_ + 4 + (acAnchorCol_ - leftCol_) * charW_, (caret_.line - topLine_) * lineH_};
    ClientToScreen(hwnd_, &origin);
    RECT work{};
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    int x = origin.x;
    int y = origin.y + lineH_ + 1;
    if (y + height > work.bottom) y = origin.y - height - 1;
    y = std::max(y, static_cast<int>(work.top));
    if (x + width > work.right) x = static_cast<int>(work.right) - width;
    x = std::max(x, static_cast<int>(work.left));
    SetWindowPos(popup, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    InvalidateRect(popup, nullptr, FALSE);
}

void CodeEditor::refreshAutocompletePos() {
    if (acItems_.empty() || !acPopup_ || !IsWindowVisible(acPopup_)) return;
    placeAutocompletePopup();
}

void CodeEditor::paintAutocompletePopup(HDC dc, const RECT &rc) {
    const Theme &t = theme();
    winutil::fillRectColor(dc, rc, t.panelBg);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, font_));
    SetBkMode(dc, TRANSPARENT);
    const int rows = static_cast<int>(std::min<std::size_t>(acItems_.size(), 8));
    for (int i = 0; i < rows; ++i) {
        const std::string &item = acItems_[static_cast<std::size_t>(i)];
        const bool sel = i == acSel_;
        const int y = 4 + i * lineH_;
        if (sel) {
            RECT rowRc{2, y, static_cast<LONG>(std::max(4, static_cast<int>(rc.right) - 2)), y + lineH_};
            winutil::fillRectColor(dc, rowRc, t.selectionBg);
        }
        const COLORREF color =
            sel ? t.selectionFg : ((!item.empty() && item.front() == '.') ? t.tokDirective : t.tokMnemonic);
        winutil::drawTextOut(dc, 10, y + 1, item, color);
    }
    SelectObject(dc, oldFont);
    RECT frame{rc.left, rc.top, rc.right - 1, rc.bottom - 1};
    winutil::frameRectColor(dc, frame, t.border);
}

void CodeEditor::paintHoverTipPopup(HDC dc, const RECT &rc) {
    const Theme &t = theme();
    winutil::fillRectColor(dc, rc, t.panelBg);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, font_));
    winutil::drawTextOut(dc, 8, 3, tipText_, t.fg);
    SelectObject(dc, oldFont);
    RECT frame{rc.left, rc.top, rc.right - 1, rc.bottom - 1};
    winutil::frameRectColor(dc, frame, t.border);
}

void CodeEditor::updateAutocomplete() {
    if (!hwnd_ || caret_.line < 0 || caret_.line >= static_cast<int>(lines_.size())) return;
    const std::string &lineRef = lines_[static_cast<std::size_t>(caret_.line)];
    const int end = std::min(caret_.col, static_cast<int>(lineRef.size()));
    int start = end;
    while (start > 0 && isWordChar(lineRef[static_cast<std::size_t>(start - 1)])) --start;
    if (end - start <= 0) {
        hideAutocomplete();
        return;
    }
    const std::string prefix =
        upperAscii(lineRef.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start)));
    std::vector<std::string> matches;
    for (const std::string &item : completionList())
        if (ciStartsWith(item, prefix)) matches.push_back(item);
    if (matches.empty()) {
        hideAutocomplete();
        return;
    }
    if (matches.size() > 8) matches.resize(8);
    acItems_ = std::move(matches);
    acSel_ = 0;
    acAnchorLine_ = caret_.line;
    acAnchorCol_ = start;
    placeAutocompletePopup();
}

void CodeEditor::hideAutocomplete() {
    if (acPopup_ && IsWindow(acPopup_) && IsWindowVisible(acPopup_)) ShowWindow(acPopup_, SW_HIDE);
    acItems_.clear();
    acSel_ = 0;
}

void CodeEditor::acceptAutocomplete() {
    if (acItems_.empty()) {
        hideAutocomplete();
        return;
    }
    const int idx = std::clamp(acSel_, 0, static_cast<int>(acItems_.size()) - 1);
    const std::string choice = acItems_[static_cast<std::size_t>(idx)];
    pushUndo();
    if (caret_.line == acAnchorLine_) {
        std::string &lineRef = lines_[static_cast<std::size_t>(caret_.line)];
        const int s = std::clamp(acAnchorCol_, 0, static_cast<int>(lineRef.size()));
        const int e = std::clamp(caret_.col, s, static_cast<int>(lineRef.size()));
        lineRef.erase(static_cast<std::size_t>(s), static_cast<std::size_t>(e - s));
        lineRef.insert(static_cast<std::size_t>(s), choice);
        caret_.col = s + static_cast<int>(choice.size());
    }
    anchor_ = caret_;
    modified_ = true;
    hideAutocomplete();
    ensureCaretVisible();
    InvalidateRect(hwnd_, nullptr, FALSE);
    notifyParent(1);
}

void CodeEditor::autoCompleteClickedAt(int /*x*/, int y) {
    const int rows = static_cast<int>(std::min<std::size_t>(acItems_.size(), 8));
    const int row = (y - 4) / std::max(1, lineH_);
    if (row >= 0 && row < rows) {
        acSel_ = row;
        acceptAutocomplete();
    } else {
        hideAutocomplete();
    }
}

void CodeEditor::updateHoverTip(int x, int y) {
    if (!symbolLookup_) {
        hideHoverTip();
        return;
    }
    const int ln = topLine_ + std::max(0, y / std::max(1, lineH_));
    if (ln < 0 || ln >= static_cast<int>(lines_.size())) {
        hideHoverTip();
        return;
    }
    const std::string &lineRef = lines_[static_cast<std::size_t>(ln)];
    int col = leftCol_ + std::max(0, x - gutterW_ - 4) / std::max(1, charW_);
    col = std::min(col, static_cast<int>(lineRef.size()));
    int start = -1;
    if (col < static_cast<int>(lineRef.size()) && isWordChar(lineRef[static_cast<std::size_t>(col)]))
        start = col;
    else if (col > 0 && isWordChar(lineRef[static_cast<std::size_t>(col - 1)]))
        start = col - 1;
    if (start < 0) {
        hideHoverTip();
        return;
    }
    int end = start;
    while (start > 0 && isWordChar(lineRef[static_cast<std::size_t>(start - 1)])) --start;
    while (end + 1 < static_cast<int>(lineRef.size()) &&
           isWordChar(lineRef[static_cast<std::size_t>(end + 1)]))
        ++end;
    const std::string word =
        lineRef.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start + 1));
    if (word != tipWord_) {
        uint16_t value = 0;
        tipFound_ = symbolLookup_(word, value);
        tipValue_ = value;
        tipWord_ = word;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s = $%04X (%u)", word.c_str(), static_cast<unsigned>(tipValue_),
                      static_cast<unsigned>(tipValue_));
        tipText_ = buf;
    }
    if (!tipFound_) {
        hideHoverTip();
        return;
    }
    HWND tip = ensurePopup(false);
    if (!tip) return;
    HDC dc = GetDC(hwnd_);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, font_));
    SIZE sz{};
    GetTextExtentPoint32A(dc, tipText_.c_str(), static_cast<int>(tipText_.size()), &sz);
    SelectObject(dc, oldFont);
    ReleaseDC(hwnd_, dc);
    const int width = std::max(80, static_cast<int>(sz.cx) + 18);
    const int height = lineH_ + 7;
    POINT origin{x, y};
    ClientToScreen(hwnd_, &origin);
    RECT work{};
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    int px = origin.x + 14;
    int py = origin.y + 20;
    if (py + height > work.bottom) py = origin.y - height - 4;
    py = std::max(py, static_cast<int>(work.top));
    if (px + width > work.right) px = static_cast<int>(work.right) - width;
    px = std::max(px, static_cast<int>(work.left));
    SetWindowPos(tip, nullptr, px, py, width, height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    InvalidateRect(tip, nullptr, FALSE);
}

void CodeEditor::hideHoverTip() {
    if (tipPopup_ && IsWindow(tipPopup_) && IsWindowVisible(tipPopup_)) ShowWindow(tipPopup_, SW_HIDE);
    tipWord_.clear();
    tipFound_ = false;
}

std::pair<const CodeEditor::ErrorMark *, const CodeEditor::ErrorMark *>
CodeEditor::marksOnLine(int line) const {
    using MarkIt = std::vector<ErrorMark>::const_iterator;
    const MarkIt lo = std::lower_bound(errorMarks_.begin(), errorMarks_.end(), line,
                                       [](const ErrorMark &m, int l) { return m.line < l; });
    if (lo == errorMarks_.end() || lo->line != line) return {nullptr, nullptr};
    MarkIt hi = lo;
    while (hi != errorMarks_.end() && hi->line == line) ++hi;
    return {&*lo, &*hi};
}

void CodeEditor::drawErrorMarksRow(HDC dc, int lineIndex, int screenY, int textOffsetX) const {
    if (errorMarks_.empty()) return;
    const auto range = marksOnLine(lineIndex);
    if (!range.first) return;
    const Theme &t = theme();
    const std::string &lineRef = lines_[static_cast<std::size_t>(lineIndex)];
    if (lineRef.empty()) return;
    for (const ErrorMark *m = range.first; m != range.second; ++m) {
        int col = std::max(0, m->column);
        if (col >= static_cast<int>(lineRef.size())) col = static_cast<int>(lineRef.size()) - 1;
        int end = col;
        while (end < static_cast<int>(lineRef.size()) && isWordChar(lineRef[static_cast<std::size_t>(end)]))
            ++end;
        if (end == col) end = std::min(col + 1, static_cast<int>(lineRef.size()));
        int x0 = textOffsetX + col * charW_;
        const int x1 = textOffsetX + end * charW_;
        x0 = std::max(x0, gutterW_ + 2);
        if (x1 - x0 >= 2) drawSquiggle(dc, x0, x1, screenY + lineH_ - 3, t.err);
    }
}

void CodeEditor::drawSquiggle(HDC dc, int x0, int x1, int y, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, x0, ((x0 / 2) & 1) != 0 ? y : y - 2, nullptr);
    for (int x = x0 + 2; x <= x1; x += 2) LineTo(dc, x, ((x / 2) & 1) != 0 ? y : y - 2);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void CodeEditor::tokenize(int lineIndex, std::vector<std::pair<int, COLORREF>> &colors) const {
    const Theme &t = theme();
    const std::string &line = lines_[static_cast<std::size_t>(lineIndex)];
    colors.assign(line.size(), {0, t.fg});
    std::size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (c == ';') {
            for (; i < line.size(); ++i) colors[i].second = t.tokComment;
            break;
        }
        if (c == '"' || c == '\'') {
            const char quote = c;
            colors[i].second = t.tokString;
            ++i;
            while (i < line.size()) {
                colors[i].second = t.tokString;
                if (line[i] == '\\' && i + 1 < line.size()) {
                    colors[i + 1].second = t.tokString;
                    i += 2;
                    continue;
                }
                if (line[i] == quote) {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || ((c == '$' || c == '%') && i + 1 < line.size())) {
            while (i < line.size() && (std::isxdigit(static_cast<unsigned char>(line[i])) || line[i] == '$' ||
                                       line[i] == '%' || line[i] == '_')) {
                colors[i].second = t.tokNumber;
                ++i;
            }
            continue;
        }
        if (isWordChar(c)) {
            const std::size_t begin = i;
            while (i < line.size() && isWordChar(line[i])) ++i;
            const std::string word = line.substr(begin, i - begin);
            const std::string wu = upperAscii(word);
            COLORREF color = t.fg;
            if (word[0] == '.')
                color = directiveSet().count(wu) ? t.tokDirective : t.tokLabel;
            else if (mnemonicSet().count(wu))
                color = t.tokMnemonic;
            else if (wu.size() == 1 && (wu == "A" || wu == "X" || wu == "Y"))
                color = t.tokRegister;
            else if (i < line.size() && line[i] == ':')
                color = t.tokLabel;
            for (std::size_t k = begin; k < i; ++k) colors[k].second = color;
            continue;
        }
        colors[i].second = t.tokPunct;
        ++i;
    }
}

void CodeEditor::drawGutter(HDC dc, int width) {
    const Theme &t = theme();
    RECT rc{0, 0, width, 20000};
    winutil::fillRectColor(dc, rc, t.gutterBg);
    HFONT old = static_cast<HFONT>(SelectObject(dc, font_));
    SetBkMode(dc, TRANSPARENT);
    char buf[16];
    const int rows = pageRows_ + 1;
    for (int r = 0; r <= rows; ++r) {
        const int ln = topLine_ + r;
        if (ln >= static_cast<int>(lines_.size())) break;
        std::snprintf(buf, sizeof(buf), "%d", ln + 1);
        SetTextColor(dc, ln == caret_.line ? t.lineNoActive : t.gutterFg);
        SIZE sz{};
        GetTextExtentPoint32A(dc, buf, static_cast<int>(strlen(buf)), &sz);
        TextOutA(dc, gutterW_ - sz.cx - 8, r * lineH_ + 1, buf, static_cast<int>(strlen(buf)));
        if (!errorMarks_.empty()) {
            const auto range = marksOnLine(ln);
            if (range.first) {
                RECT dot{3, r * lineH_ + lineH_ / 2 - 3, 9, r * lineH_ + lineH_ / 2 + 3};
                winutil::fillRectColor(dc, dot, t.err);
            }
        }
    }
    SelectObject(dc, old);
}

void CodeEditor::drawLineText(HDC dc, int lineIndex, int screenY, int textOffsetX) {
    const Theme &t = theme();
    const std::string &line = lines_[static_cast<std::size_t>(lineIndex)];
    std::vector<std::pair<int, COLORREF>> colors;
    tokenize(lineIndex, colors);
    HFONT old = static_cast<HFONT>(SelectObject(dc, font_));
    SetBkMode(dc, TRANSPARENT);
    int runStart = 0;
    COLORREF runColor = colors.empty() ? t.fg : colors.front().second;
    auto flushRun = [&](int endCol) {
        if (endCol <= runStart) return;
        const int startPx = textOffsetX + (runStart - leftCol_) * charW_;
        if (startPx + (endCol - runStart) * charW_ < gutterW_) return;
        const std::string chunk =
            line.substr(static_cast<std::size_t>(runStart), static_cast<std::size_t>(endCol - runStart));
        winutil::drawTextOut(dc, startPx, screenY, chunk, runColor);
    };
    for (int col = 0; col < static_cast<int>(colors.size()); ++col) {
        if (colors[static_cast<std::size_t>(col)].second != runColor) {
            flushRun(col);
            runStart = col;
            runColor = colors[static_cast<std::size_t>(col)].second;
        }
    }
    flushRun(static_cast<int>(colors.size()));
    SelectObject(dc, old);
}

void CodeEditor::paintBuffer(HDC buffer, const RECT &full) {
    const Theme &t = theme();
    winutil::fillRectColor(buffer, full, t.panelBg);
    drawGutter(buffer, gutterW_);

    const int textOffsetX = gutterW_ + 4 - leftCol_ * charW_;
    const int rows = pageRows_ + 1;

    Caret selA = anchor_;
    Caret selB = caret_;
    if (selA.line > selB.line || (selA.line == selB.line && selA.col > selB.col)) std::swap(selA, selB);
    const bool selection = hasSelection();

    for (int r = 0; r <= rows; ++r) {
        const int ln = topLine_ + r;
        if (ln >= static_cast<int>(lines_.size())) break;
        const int y = r * lineH_;

        if (ln == caret_.line && !selection) {
            RECT curLineRect{gutterW_, y, full.right, y + lineH_};
            winutil::fillRectColor(buffer, curLineRect, t.curLine);
        }

        if (selection) {
            const int fromCol = (ln == selA.line) ? selA.col : 0;
            const int toCol =
                (ln == selB.line) ? selB.col : static_cast<int>(lines_[static_cast<std::size_t>(ln)].size());
            if ((ln >= selA.line && ln <= selB.line) && fromCol < toCol) {
                RECT selRect{textOffsetX + fromCol * charW_, y, textOffsetX + toCol * charW_, y + lineH_};
                if (selRect.left < gutterW_) selRect.left = gutterW_;
                winutil::fillRectColor(buffer, selRect, t.selectionBg);
            }
        }

        drawLineText(buffer, ln, y + 1, textOffsetX);
        drawErrorMarksRow(buffer, ln, y, textOffsetX);
    }

    if (GetFocus() == hwnd_) {
        HideCaret(hwnd_);
        CreateCaret(hwnd_, nullptr, 2, lineH_ - 4);
        SetCaretPos(gutterW_ + 4 + (caret_.col - leftCol_) * charW_, (caret_.line - topLine_) * lineH_ + 2);
        ShowCaret(hwnd_);
    }
}

void CodeEditor::onPaint(HDC dc, const RECT &rcPaint) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = std::max(1, static_cast<int>(rc.right));
    const int h = std::max(1, static_cast<int>(rc.bottom));
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(mem, bmp));
    paintBuffer(mem, rc);
    BitBlt(dc, rcPaint.left, rcPaint.top, rcPaint.right - rcPaint.left, rcPaint.bottom - rcPaint.top, mem,
           rcPaint.left, rcPaint.top, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}
