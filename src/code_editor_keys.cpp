#include "code_editor.hpp"
#include "theme.hpp"
#include "win_util.hpp"

#include <algorithm>
#include <cctype>

namespace {

bool wordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '@';
}

char matchingCloser(char open) {
    switch (open) {
    case '(':
        return ')';
    case '[':
        return ']';
    case '{':
        return '}';
    case '"':
        return '"';
    default:
        return 0;
    }
}

} // namespace

LRESULT CodeEditor::handleKey(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CHAR) {
        const char ch = static_cast<char>(wParam);
        if ((ch == '\r' || ch == '\t') && !acItems_.empty()) {
            acceptAutocomplete();
            return 0;
        }
        switch (ch) {
        case '\r':
        case '\n': {
            pushUndo();
            deleteSelection();
            auto &line = lines_[static_cast<std::size_t>(caret_.line)];
            const auto firstNonWs = line.find_first_not_of(" \t");
            std::string indent = (firstNonWs == std::string::npos) ? line : line.substr(0, firstNonWs);
            std::string tail = line.substr(static_cast<std::size_t>(caret_.col));
            char closer = caret_.col > 0 ? matchingCloser(line[static_cast<std::size_t>(caret_.col - 1)]) : 0;
            if (closer != 0 && !tail.empty() && std::isalnum(static_cast<unsigned char>(tail.front())) != 0)
                closer = 0;
            std::string inserted = indent;
            if (closer != 0) inserted += closer;
            inserted += tail;
            line.resize(static_cast<std::size_t>(caret_.col));
            lines_.insert(lines_.begin() + caret_.line + 1, inserted);
            caret_.line += 1;
            caret_.col = static_cast<int>(indent.size());
            anchor_ = caret_;
            modified_ = true;
            ensureCaretVisible();
            InvalidateRect(hwnd_, nullptr, FALSE);
            notifyParent(1);
            return 0;
        }
        case '\t': {
            pushUndo();
            deleteSelection();
            auto &line = lines_[static_cast<std::size_t>(caret_.line)];
            line.insert(static_cast<std::size_t>(caret_.col), 4, ' ');
            caret_.col += 4;
            anchor_ = caret_;
            modified_ = true;
            ensureCaretVisible();
            InvalidateRect(hwnd_, nullptr, FALSE);
            notifyParent(1);
            return 0;
        }
        case '\b': {
            if (!hasSelection() && caret_.col == 0 && caret_.line == 0) return 0;
            pushUndo();
            if (hasSelection()) {
                deleteSelection();
            } else if (caret_.col > 0) {
                auto &line = lines_[static_cast<std::size_t>(caret_.line)];
                line.erase(static_cast<std::size_t>(caret_.col - 1), 1);
                --caret_.col;
            } else {
                auto &prev = lines_[static_cast<std::size_t>(caret_.line - 1)];
                caret_.col = static_cast<int>(prev.size());
                prev += lines_[static_cast<std::size_t>(caret_.line)];
                lines_.erase(lines_.begin() + caret_.line);
                --caret_.line;
            }
            anchor_ = caret_;
            modified_ = true;
            ensureCaretVisible();
            updateAutocomplete();
            InvalidateRect(hwnd_, nullptr, FALSE);
            notifyParent(1);
            return 0;
        }
        default: {
            if (static_cast<unsigned char>(ch) < 32 || ch == 127) return 0;
            const char closer = matchingCloser(ch);
            const std::string &lineRef = lines_[static_cast<std::size_t>(caret_.line)];
            if (!hasSelection() && closer != 0 && caret_.col < static_cast<int>(lineRef.size()) &&
                lineRef[static_cast<std::size_t>(caret_.col)] == ch) {
                ++caret_.col;
                anchor_ = caret_;
                ensureCaretVisible();
                InvalidateRect(hwnd_, nullptr, FALSE);
                notifyParent(2);
                return 0;
            }
            const bool nextIsAlnum =
                !hasSelection() && caret_.col < static_cast<int>(lineRef.size()) &&
                std::isalnum(static_cast<unsigned char>(lineRef[static_cast<std::size_t>(caret_.col)])) != 0;
            pushUndo();
            if (hasSelection() && closer != 0) {
                const std::string sel = selectedText();
                deleteSelection();
                std::string wrapped;
                wrapped += ch;
                wrapped += sel;
                wrapped += closer;
                insertTextAt(wrapped, false);
            } else if (closer != 0 && !nextIsAlnum) {
                auto &line = lines_[static_cast<std::size_t>(caret_.line)];
                std::string pair;
                pair += ch;
                pair += closer;
                line.insert(static_cast<std::size_t>(caret_.col), pair);
                ++caret_.col;
            } else {
                deleteSelection();
                auto &line = lines_[static_cast<std::size_t>(caret_.line)];
                line.insert(line.begin() + caret_.col, ch);
                ++caret_.col;
            }
            anchor_ = caret_;
            modified_ = true;
            ensureCaretVisible();
            if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.')
                updateAutocomplete();
            else
                hideAutocomplete();
            InvalidateRect(hwnd_, nullptr, FALSE);
            notifyParent(1);
            return 0;
        }
        }
    }

    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return DefWindowProcA(hwnd_, msg, wParam, lParam);

    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    if (!acItems_.empty()) {
        const int n = static_cast<int>(acItems_.size());
        switch (wParam) {
        case VK_ESCAPE:
            hideAutocomplete();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case VK_UP:
            acSel_ = (acSel_ + n - 1) % n;
            InvalidateRect(acPopup_, nullptr, FALSE);
            return 0;
        case VK_DOWN:
            acSel_ = (acSel_ + 1) % n;
            InvalidateRect(acPopup_, nullptr, FALSE);
            return 0;
        case VK_PRIOR:
            acSel_ = 0;
            InvalidateRect(acPopup_, nullptr, FALSE);
            return 0;
        case VK_NEXT:
            acSel_ = n - 1;
            InvalidateRect(acPopup_, nullptr, FALSE);
            return 0;
        case VK_LEFT:
        case VK_RIGHT:
        case VK_HOME:
        case VK_END:
        case VK_DELETE:
            hideAutocomplete();
            break;
        default:
            break;
        }
    }

    auto moveCaret = [&](int newLine, int newCol, bool select) {
        caret_.line = newLine;
        caret_.col = newCol;
        clampCaret();
        if (!select) anchor_ = caret_;
        ensureCaretVisible();
        InvalidateRect(hwnd_, nullptr, FALSE);
        notifyParent(2);
    };

    auto &line = lines_[static_cast<std::size_t>(caret_.line)];

    switch (wParam) {
    case VK_LEFT: {
        int nl = caret_.line;
        int nc = caret_.col;
        if (ctrl) {
            while (nc > 0 && !wordChar(line[static_cast<std::size_t>(nc - 1)])) --nc;
            while (nc > 0 && wordChar(line[static_cast<std::size_t>(nc - 1)])) --nc;
        } else if (caret_.col > 0) {
            --nc;
        } else if (caret_.line > 0) {
            --nl;
            nc = static_cast<int>(lines_[static_cast<std::size_t>(nl)].size());
        }
        moveCaret(nl, nc, shift);
        return 0;
    }
    case VK_RIGHT: {
        int nl = caret_.line;
        int nc = caret_.col;
        if (ctrl) {
            while (nc < static_cast<int>(line.size()) && !wordChar(line[static_cast<std::size_t>(nc)])) ++nc;
            while (nc < static_cast<int>(line.size()) && wordChar(line[static_cast<std::size_t>(nc)])) ++nc;
        } else if (caret_.col < static_cast<int>(line.size())) {
            ++nc;
        } else if (caret_.line < static_cast<int>(lines_.size()) - 1) {
            ++nl;
            nc = 0;
        }
        moveCaret(nl, nc, shift);
        return 0;
    }
    case VK_UP: {
        if (ctrl && !shift) {
            moveLines(-1);
            return 0;
        }
        const int target = std::max(0, caret_.line - (ctrl ? pageRows_ - 2 : 1));
        moveCaret(target, caret_.col, shift);
        return 0;
    }
    case VK_DOWN: {
        if (ctrl && !shift) {
            moveLines(1);
            return 0;
        }
        const int maxLine = static_cast<int>(lines_.size()) - 1;
        const int target = std::min(maxLine, caret_.line + (ctrl ? pageRows_ - 2 : 1));
        moveCaret(target, caret_.col, shift);
        return 0;
    }
    case VK_HOME: {
        int col = 0;
        if (!ctrl) {
            const auto firstNonWs = line.find_first_not_of(" \t");
            const int smartCol = firstNonWs == std::string::npos ? 0 : static_cast<int>(firstNonWs);
            col = (caret_.col == smartCol) ? 0 : smartCol;
        }
        moveCaret(ctrl ? 0 : caret_.line, ctrl ? 0 : col, shift);
        return 0;
    }
    case VK_END: {
        moveCaret(ctrl ? static_cast<int>(lines_.size()) - 1 : caret_.line,
                  ctrl ? static_cast<int>(lines_.back().size()) : static_cast<int>(line.size()), shift);
        return 0;
    }
    case VK_PRIOR:
        moveCaret(std::max(0, caret_.line - pageRows_), caret_.col, shift);
        return 0;
    case VK_NEXT:
        moveCaret(std::min(static_cast<int>(lines_.size()) - 1, caret_.line + pageRows_), caret_.col, shift);
        return 0;
    case VK_BACK: {
        SendMessageA(hwnd_, WM_CHAR, '\b', 0);
        return 0;
    }
    case VK_DELETE: {
        if (!hasSelection() && caret_.col >= static_cast<int>(line.size()) &&
            caret_.line >= static_cast<int>(lines_.size()) - 1)
            return 0;
        pushUndo();
        hideAutocomplete();
        if (hasSelection()) {
            deleteSelection();
        } else if (caret_.col < static_cast<int>(line.size())) {
            line.erase(static_cast<std::size_t>(caret_.col), 1);
        } else {
            line += lines_[static_cast<std::size_t>(caret_.line + 1)];
            lines_.erase(lines_.begin() + caret_.line + 1);
        }
        modified_ = true;
        ensureCaretVisible();
        InvalidateRect(hwnd_, nullptr, FALSE);
        notifyParent(1);
        return 0;
    }
    case 'A':
        if (ctrl) {
            selectAll();
            return 0;
        }
        break;
    case 'C':
        if (ctrl) {
            copy(false);
            return 0;
        }
        break;
    case 'X':
        if (ctrl) {
            copy(true);
            return 0;
        }
        break;
    case 'V':
        if (ctrl) {
            paste();
            return 0;
        }
        break;
    case 'Z':
        if (ctrl) {
            undo();
            return 0;
        }
        break;
    case 'Y':
        if (ctrl) {
            redo();
            return 0;
        }
        break;
    case 'D':
        if (ctrl) {
            duplicateLine();
            return 0;
        }
        break;
    case VK_OEM_2:
        if (ctrl) {
            toggleComment();
            return 0;
        }
        break;
    default:
        break;
    }

    if (msg == WM_KEYDOWN && wParam == VK_TAB && shift) {
        pushUndo();
        Caret a = anchor_;
        Caret b = caret_;
        if (a.line > b.line) std::swap(a, b);
        for (int ln = a.line; ln <= b.line; ++ln) {
            auto &l = lines_[static_cast<std::size_t>(ln)];
            int removeCount = 0;
            while (removeCount < 4 && removeCount < static_cast<int>(l.size()) &&
                   l[static_cast<std::size_t>(removeCount)] == ' ')
                ++removeCount;
            if (removeCount > 0) {
                l.erase(0, static_cast<std::size_t>(removeCount));
                if (ln == caret_.line) caret_.col = std::max(0, caret_.col - removeCount);
                if (ln == anchor_.line) anchor_.col = std::max(0, anchor_.col - removeCount);
            }
        }
        modified_ = true;
        ensureCaretVisible();
        InvalidateRect(hwnd_, nullptr, FALSE);
        notifyParent(1);
        return 0;
    }

    return DefWindowProcA(hwnd_, msg, wParam, lParam);
}
