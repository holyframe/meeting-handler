#include "WindowPickOutline.h"

#include <dwmapi.h>
#include <QPalette>
#include <QRegion>

namespace {
bool placeAtFrame(HWND outline, const RECT& frame) {
    RECT current{};
    if (::GetWindowRect(outline, &current) && ::EqualRect(&current, &frame)) return true;
    return ::SetWindowPos(outline, HWND_TOPMOST, frame.left, frame.top,
                          frame.right - frame.left, frame.bottom - frame.top,
                          SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE;
}
}

WindowPickOutline::WindowPickOutline(QWidget* owner)
    : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                     Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint) {
    setObjectName("captionTargetOutline");
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    QPalette colors = palette();
    colors.setColor(QPalette::Window, Qt::red);
    setPalette(colors);
    setAutoFillBackground(true);
}

void WindowPickOutline::showAround(HWND target) {
    RECT frame{};
    if (!target || !::IsWindowVisible(target) || ::IsIconic(target) ||
        ((FAILED(::DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) ||
          ::IsRectEmpty(&frame)) && !::GetWindowRect(target, &frame)) || ::IsRectEmpty(&frame)) {
        hide();
        return;
    }

    // Both the browser frame and the top-level overlay use native desktop pixels.
    const auto outline = reinterpret_cast<HWND>(winId());
    const QSize size(frame.right - frame.left, frame.bottom - frame.top);
    if (!placeAtFrame(outline, frame) || !setFrameRegion(outline, size)) {
        hide();
        return;
    }
    if (!isVisible()) {
        show();
        // Showing a Qt tool window can restore its previous placement.
        if (!placeAtFrame(outline, frame)) hide();
    }
}

bool WindowPickOutline::setFrameRegion(HWND outline, const QSize& size) {
    const int border = qMax(1, ::MulDiv(3, ::GetDpiForWindow(outline), USER_DEFAULT_SCREEN_DPI));
    if (isVisible() && size == m_regionSize && border == m_border) return true;

    // Shape the window in physical pixels. A fractional Qt scale must not round
    // a full-screen border onto the adjacent monitor or cover the target's interior.
    const QRect outer(QPoint(), size);
    const QRegion ring = QRegion(outer).subtracted(outer.adjusted(border, border, -border, -border));
    HRGN region = ring.toHRGN();
    if (!region) return false;
    if (!::SetWindowRgn(outline, region, TRUE)) {
        ::DeleteObject(region); // Windows takes ownership only on success.
        return false;
    }
    m_regionSize = size;
    m_border = border;
    return true;
}
