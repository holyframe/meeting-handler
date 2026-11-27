#pragma once

#include "captions/Settings.h"
#include <QTimer>
#include <windows.h>
#include <functional>
#include <map>
#include <memory>

class WindowsTop final : public QObject {
    Q_OBJECT
public:
    struct DesktopWindows {
        std::function<HWND()> foreground = ::GetForegroundWindow;
        std::function<HWND()> underPointer = [] {
            POINT point{};
            return ::GetCursorPos(&point) ? ::WindowFromPoint(point) : nullptr;
        };
    };
    explicit WindowsTop(DesktopWindows desktop);
    WindowsTop() : WindowsTop(DesktopWindows{}) {}
    ~WindowsTop();
    void configure(const WindowsTopSettings& settings);
    // An empty result means the command succeeded.
    QString toggle(WindowAction action, HWND window);
    bool isPinned(HWND window) const;
    QString setPinned(HWND window, bool pinned);
    QString toggleForeground(WindowAction action) { return toggle(action, m_desktop.foreground()); }
    static QString actionName(WindowAction action);

signals:
    void pinChanged(quintptr window, bool pinned);

private:
    struct Window;
    bool owns(HWND window) const;
    HWND hoveredWindow() const;
    bool updatePin(HWND hwnd, Window& window);
    void refresh();
    void forgetUnused();
    WindowsTopSettings m_settings;
    std::wstring m_property;
    DesktopWindows m_desktop;
    std::map<HWND, std::unique_ptr<Window>> m_windows;
    QTimer m_timer;
};
