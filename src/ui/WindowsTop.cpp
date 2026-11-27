#include "WindowsTop.h"
#include "WindowPickOutline.h"
#include "WindowPreview.h"

#include <optional>

namespace {
bool isTopmost(HWND window) {
    return (::GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

bool setTopmost(HWND window, bool on, bool restoring = false) {
    // Queue the final restoration even if a previous cross-thread change is still pending.
    if (!restoring && on == isTopmost(window)) return true;
    return ::SetWindowPos(window, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                          SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS) != FALSE;
}

bool setLayered(HWND window, bool on) {
    const LONG_PTR style = ::GetWindowLongPtrW(window, GWL_EXSTYLE);
    ::SetLastError(ERROR_SUCCESS);
    const auto previous = ::SetWindowLongPtrW(window, GWL_EXSTYLE, on ? style | WS_EX_LAYERED : style & ~WS_EX_LAYERED);
    return previous != 0 || ::GetLastError() == ERROR_SUCCESS;
}

struct OriginalOpacity {
    bool layered;
    COLORREF key = 0;
    BYTE alpha = 255;
    DWORD flags = 0;
    int appliedAlpha = -1;

    static std::optional<OriginalOpacity> capture(HWND window) {
        OriginalOpacity state{(::GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED) != 0};
        // Per-pixel layered windows cannot be restored using a constant alpha.
        if (state.layered && !::GetLayeredWindowAttributes(window, &state.key, &state.alpha, &state.flags)) return {};
        if (!state.layered && !setLayered(window, true)) return {};
        return state;
    }

    bool apply(HWND window, int alphaValue) {
        if (appliedAlpha == alphaValue) return true;
        if (!::SetLayeredWindowAttributes(window, key, static_cast<BYTE>(alphaValue), flags | LWA_ALPHA)) return false;
        appliedAlpha = alphaValue;
        return true;
    }

    bool restore(HWND window) const {
        if (layered) return ::SetLayeredWindowAttributes(window, key, alpha, flags) != FALSE;
        if (!setLayered(window, false)) return false;
        ::RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_ERASE);
        return true;
    }
};

bool eligible(HWND window) {
    if (!window || window == ::GetDesktopWindow() || window == ::GetShellWindow() || !::IsWindowVisible(window)) return false;
    wchar_t name[64]{};
    ::GetClassNameW(window, name, int(std::size(name)));
    for (const auto* shell : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW"})
        if (!wcscmp(name, shell)) return false;
    return !dynamic_cast<WindowPickOutline*>(QWidget::find(reinterpret_cast<WId>(window)));
}
}

struct WindowsTop::Window {
    bool pin = false;
    bool originalTopmost = false;
    std::unique_ptr<WindowPickOutline> outline;
    std::optional<OriginalOpacity> opacity;
    std::unique_ptr<WindowPreview> preview;
};

WindowsTop::WindowsTop(DesktopWindows desktop)
    : m_property(L"MeetingHandler.WindowsTop." + std::to_wstring(::GetCurrentProcessId()) + L"." +
                 std::to_wstring(reinterpret_cast<uintptr_t>(this))), m_desktop(std::move(desktop)) {
    m_timer.setInterval(80);
    QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] { refresh(); });
}

WindowsTop::~WindowsTop() {
    m_timer.stop();
    for (auto& [hwnd, state] : m_windows) {
        state->preview.reset();
        state->outline.reset();
        if (!owns(hwnd)) continue;
        if (state->pin) setTopmost(hwnd, state->originalTopmost, true);
        if (state->opacity) state->opacity->restore(hwnd);
        ::RemovePropW(hwnd, m_property.c_str());
    }
}

bool WindowsTop::owns(HWND window) const {
    // Windows removes the property when the window dies, even if its HWND is later reused.
    return ::GetPropW(window, m_property.c_str()) == this;
}

HWND WindowsTop::hoveredWindow() const {
    return ::GetAncestor(m_desktop.underPointer(), GA_ROOT);
}

bool WindowsTop::isPinned(HWND window) const {
    const auto it = m_windows.find(window);
    return it != m_windows.end() && it->second->pin && owns(window);
}

QString WindowsTop::setPinned(HWND window, bool pinned) {
    if (pinned == isPinned(window)) return {};
    return toggle(WindowAction::Pin, window);
}

void WindowsTop::configure(const WindowsTopSettings& settings) {
    m_settings = settings;
    for (auto& [hwnd, state] : m_windows) {
        if (state->preview) state->preview->setInteractive(settings.interactivePreview);
    }
    refresh();
}

QString WindowsTop::actionName(WindowAction action) {
    switch (action) {
    case WindowAction::Pin: return QObject::tr("Always on Top");
    case WindowAction::PictureInPicture: return QObject::tr("Picture in Picture");
    case WindowAction::Crop: return QObject::tr("Crop preview");
    case WindowAction::Opacity: return QObject::tr("Window opacity");
    default: return {};
    }
}

bool WindowsTop::updatePin(HWND hwnd, Window& window) {
    const bool suspend = m_settings.suspendWhenMaximized && ::IsZoomed(hwnd);
    if (!setTopmost(hwnd, suspend ? window.originalTopmost : true)) return false;
    if (m_settings.highlight && !suspend && isTopmost(hwnd)) {
        if (!window.outline) {
            window.outline = std::make_unique<WindowPickOutline>(nullptr);
            window.outline->setObjectName("pinnedWindowBorder");
            window.outline->setAttribute(Qt::WA_QuitOnClose, false);
        }
        window.outline->setAppearance(QColor::fromRgb(m_settings.borderRgb), m_settings.borderWidth);
        window.outline->showAround(hwnd, true);
    } else {
        window.outline.reset();
    }
    return true;
}

QString WindowsTop::toggle(WindowAction action, HWND candidate) {
    HWND hwnd = ::GetAncestor(candidate, GA_ROOT);
    for (const auto& [source, state] : m_windows) {
        if (state->preview && reinterpret_cast<HWND>(state->preview->winId()) == hwnd) {
            if (!owns(source)) return QObject::tr("The source window has closed.");
            hwnd = source;
            break;
        }
    }
    if (!eligible(hwnd)) return QObject::tr("Select an application window and try the shortcut again.");
    auto& slot = m_windows[hwnd];
    if (!slot || !owns(hwnd)) {
        slot = std::make_unique<Window>();
        if (!::SetPropW(hwnd, m_property.c_str(), this)) {
            m_windows.erase(hwnd);
            return QObject::tr("Windows did not allow Meeting Handler to control this window.");
        }
    }
    auto& state = *slot;
    bool ok = true;
    switch (action) {
    case WindowAction::Pin:
        if (state.pin) {
            ok = setTopmost(hwnd, state.originalTopmost, true);
            if (ok) { state.pin = false; state.outline.reset(); }
        } else {
            state.originalTopmost = isTopmost(hwnd);
            state.pin = ok = updatePin(hwnd, state);
        }
        break;
    case WindowAction::Opacity:
        if (state.opacity) {
            ok = state.opacity->restore(hwnd);
            if (ok) state.opacity.reset();
        } else {
            state.opacity = OriginalOpacity::capture(hwnd);
            const int alpha = m_settings.opaqueOnHover && hoveredWindow() == hwnd ? 255 :
                              ::MulDiv(m_settings.opacityPercent, 255, 100);
            ok = state.opacity && state.opacity->apply(hwnd, alpha);
            if (!ok && state.opacity && state.opacity->restore(hwnd)) state.opacity.reset();
        }
        break;
    case WindowAction::PictureInPicture:
    case WindowAction::Crop:
        if (state.preview && !state.preview->isVisible()) state.preview.reset();
        if (state.preview && action == WindowAction::PictureInPicture) {
            state.preview.reset();
        } else {
            if (!state.preview) {
                state.preview = std::make_unique<WindowPreview>(hwnd, [this, hwnd] { return owns(hwnd); });
                state.preview->setInteractive(m_settings.interactivePreview);
                ok = state.preview->open();
                if (!ok) state.preview.reset();
            }
            if (ok && action == WindowAction::Crop) state.preview->beginCrop();
        }
        break;
    default: ok = false;
    }
    forgetUnused();
    if (!m_windows.empty() && !m_timer.isActive()) m_timer.start();
    if (ok && action == WindowAction::Pin)
        emit pinChanged(reinterpret_cast<quintptr>(hwnd), isPinned(hwnd));
    return ok ? QString() : QObject::tr("%1 is unavailable for this window.").arg(actionName(action));
}

void WindowsTop::refresh() {
    const HWND hovered = m_settings.opaqueOnHover ? hoveredWindow() : nullptr;
    for (auto& [hwnd, state] : m_windows) {
        if (!owns(hwnd)) continue;
        if (state->pin) updatePin(hwnd, *state);
        if (state->opacity) state->opacity->apply(hwnd, hovered == hwnd ? 255 : ::MulDiv(m_settings.opacityPercent, 255, 100));
        if (state->preview && state->preview->isVisible()) state->preview->refresh();
    }
    forgetUnused();
}

void WindowsTop::forgetUnused() {
    for (auto it = m_windows.begin(); it != m_windows.end();) {
        auto& state = *it->second;
        if (state.preview && !state.preview->isVisible()) state.preview.reset();
        const bool valid = owns(it->first);
        if (!valid && state.pin) emit pinChanged(reinterpret_cast<quintptr>(it->first), false);
        if (valid && (state.pin || state.opacity || state.preview)) { ++it; continue; }
        if (valid) ::RemovePropW(it->first, m_property.c_str());
        it = m_windows.erase(it);
    }
    if (m_windows.empty()) m_timer.stop();
}
