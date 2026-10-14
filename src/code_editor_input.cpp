#include "code_editor.hpp"
#include "theme.hpp"
#include "win_util.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) static_cast<int>(static_cast<short>(LOWORD(lp)))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) static_cast<int>(static_cast<short>(HIWORD(lp)))
#endif

namespace {

bool isWordCharKey(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '@';
}

} // namespace

LRESULT CodeEditor::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        onPaint(dc, ps.rcPaint);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_SETFOCUS:
        CreateCaret(hwnd_, nullptr, 2, lineH_ - 4);
        ShowCaret(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        notifyParent(2);
        return 0;
    case WM_KILLFOCUS:
        HideCaret(hwnd_);
        DestroyCaret();
        hideAutocomplete();
        hideHoverTip();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_DESTROY:
        if (acPopup_) DestroyWindow(acPopup_);
        if (tipPopup_) DestroyWindow(tipPopup_);
        acPopup_ = nullptr;
        tipPopup_ = nullptr;
        return 0;
    case WM_MOVE:
        hideHoverTip();
        refreshAutocompletePos();
        return 0;
    case WM_SIZE:
        hideHoverTip();
        updateScrollbars();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_VSCROLL: {
        const int action = LOWORD(wParam);
        switch (action) {
        case SB_LINEUP:
            --topLine_;
            break;
        case SB_LINEDOWN:
            ++topLine_;
            break;
        case SB_PAGEUP:
            topLine_ -= pageRows_;
            break;
        case SB_PAGEDOWN:
            topLine_ += pageRows_;
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            topLine_ = HIWORD(wParam);
            break;
        default:
            break;
        }
        topLine_ = std::max(0, topLine_);
        hideHoverTip();
        refreshAutocompletePos();
        updateScrollbars();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_HSCROLL: {
        const int action = LOWORD(wParam);
        switch (action) {
        case SB_LINELEFT:
            --leftCol_;
            break;
        case SB_LINERIGHT:
            ++leftCol_;
            break;
        case SB_PAGELEFT:
            leftCol_ -= 10;
            break;
        case SB_PAGERIGHT:
            leftCol_ += 10;
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            leftCol_ = HIWORD(wParam);
            break;
        default:
            break;
        }
        leftCol_ = std::max(0, leftCol_);
        hideHoverTip();
        refreshAutocompletePos();
        updateScrollbars();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (wParam & MK_CONTROL) {
            ::PostMessageA(GetParent(hwnd_), WM_EDITOR_ZOOM, 0, delta > 0 ? 1 : -1);
            return 0;
        }
        topLine_ -= (delta / WHEEL_DELTA) * 3;
        topLine_ = std::max(0, topLine_);
        hideHoverTip();
        refreshAutocompletePos();
        updateScrollbars();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!trackingMouseLeave_) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd_;
            TrackMouseEvent(&tme);
            trackingMouseLeave_ = true;
        }
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        if (dragging_ || (wParam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) {
            hideHoverTip();
            if (!dragging_) return 0;
            const int ln = topLine_ + std::max(0, y / lineH_);
            const int col = leftCol_ + std::max(0, x - gutterW_ - 4) / std::max(1, charW_);
            caret_ = {std::min(ln, static_cast<int>(lines_.size()) - 1), col};
            clampCaret();
            ensureCaretVisible();
            InvalidateRect(hwnd_, nullptr, FALSE);
            notifyParent(2);
            return 0;
        }
        updateHoverTip(x, y);
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingMouseLeave_ = false;
        hideHoverTip();
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd_);
        SetCapture(hwnd_);
        dragging_ = true;
        hideAutocomplete();
        hideHoverTip();
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        const int ln = topLine_ + std::max(0, y / lineH_);
        const int col = leftCol_ + std::max(0, x - gutterW_ - 4) / std::max(1, charW_);
        caret_ = {std::min(ln, static_cast<int>(lines_.size()) - 1), col};
        clampCaret();
        if (!(wParam & MK_SHIFT)) anchor_ = caret_;
        ensureCaretVisible();
        InvalidateRect(hwnd_, nullptr, FALSE);
        notifyParent(2);
        return 0;
    }
    case WM_CAPTURECHANGED:
        dragging_ = false;
        return 0;
    case WM_LBUTTONUP:
        dragging_ = false;
        ReleaseCapture();
        return 0;
    case WM_LBUTTONDBLCLK: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        const int ln = topLine_ + std::max(0, y / lineH_);
        if (ln >= 0 && ln < static_cast<int>(lines_.size())) {
            auto &line = lines_[static_cast<std::size_t>(ln)];
            int col = leftCol_ + std::max(0, x - gutterW_ - 4) / std::max(1, charW_);
            col = std::min(col, static_cast<int>(line.size()) - 1);
            if (col < 0) col = 0;
            int start = col;
            int end = col;
            while (start > 0 && isWordCharKey(line[static_cast<std::size_t>(start - 1)])) --start;
            while (end < static_cast<int>(line.size()) && isWordCharKey(line[static_cast<std::size_t>(end)]))
                ++end;
            anchor_ = {ln, start};
            caret_ = {ln, end};
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    default:
        return handleKey(msg, wParam, lParam);
    }
}
