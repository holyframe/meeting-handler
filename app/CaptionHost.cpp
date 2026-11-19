#include "CaptionHost.h"

#include <QApplication>
#include <QFocusEvent>

namespace {
HWND captionTabStop(HWND pane, bool last) {
    HWND first = ::GetNextDlgTabItem(pane, nullptr, FALSE);
    return first && last ? ::GetNextDlgTabItem(pane, first, TRUE) : first;
}
}

CaptionHost::CaptionHost(QWidget* parent) : QWidget(parent) {
    setObjectName("liveCaptionHost");
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(660, 175);
}

bool CaptionHost::initialize(const Settings& settings) {
    if (!pane_.Create(reinterpret_cast<HWND>(winId()), settings, [this](const std::wstring& title) {
            window()->setWindowTitle(QStringLiteral("Meeting Handler — ") + QString::fromStdWString(title));
        })) return false;

    fitPane();
    qApp->installNativeEventFilter(this);
    return true;
}

CaptionHost::~CaptionHost() {
    qApp->removeNativeEventFilter(this);
}

void CaptionHost::fitPane() {
    if (!pane_.Handle()) return;
    // Qt sizes widgets in DIPs. Parent and child HWNDs use physical pixels.
    RECT client{};
    if (::GetClientRect(::GetParent(pane_.Handle()), &client)) {
        ::SetWindowPos(pane_.Handle(), nullptr, 0, 0, client.right, client.bottom,
                       SWP_NOACTIVATE | SWP_NOZORDER);
    }
}

void CaptionHost::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
    const HWND pane = pane_.Handle();
    if (!pane || ::IsChild(pane, ::GetFocus())) return;
    HWND target = nullptr;
    if (event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason)
        target = captionTabStop(pane, event->reason() == Qt::BacktabFocusReason);
    ::SetFocus(target ? target : pane);
}

bool CaptionHost::nativeEventFilter(const QByteArray& type, void* message, qintptr* result) {
    if (type != "windows_generic_MSG" || !pane_.Handle()) return false;
    auto* msg = static_cast<MSG*>(message);
    const HWND pane = pane_.Handle();
    // Qt can deliver a widget resize before applying the new native geometry
    // on first show. WM_SIZE has the host's committed client dimensions.
    if (msg->hwnd == ::GetParent(pane) &&
        (msg->message == WM_SIZE || msg->message == WM_DPICHANGED_AFTERPARENT))
        fitPane();
    if (msg->hwnd == ::GetAncestor(pane, GA_ROOT) && msg->message == WM_SETTINGCHANGE) {
        ::SendMessageW(pane, msg->message, msg->wParam, msg->lParam);
    }
    if (msg->message < WM_KEYFIRST || msg->message > WM_KEYLAST ||
        (msg->hwnd != pane && !::IsChild(pane, msg->hwnd))) return false;

    // Keep dialog navigation local to the native controls. At the ends of the
    // caption tab order, let Qt continue into the rest of the shared window.
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_TAB) {
        const bool backward = ::GetKeyState(VK_SHIFT) < 0;
        const HWND boundary = captionTabStop(pane, !backward);
        if (boundary && (msg->hwnd == boundary || ::IsChild(boundary, msg->hwnd)) &&
            focusNextPrevChild(!backward)) {
            if (result) *result = 0;
            return true;
        }
    }
    if (!::IsDialogMessageW(pane, msg)) return false;
    if (result) *result = 0;
    return true;
}
