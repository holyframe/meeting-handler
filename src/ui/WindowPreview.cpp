#include "WindowPreview.h"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>

namespace {
const QRectF fullWindow{0, 0, 1, 1};

RECT pixels(const QRectF& r, qreal sx, qreal sy) {
    return {qRound(r.left() * sx), qRound(r.top() * sy), qRound(r.right() * sx), qRound(r.bottom() * sy)};
}

HWND childWindowAt(HWND root, POINT screen) {
    HWND target = root;
    for (;;) {
        POINT pt = screen;
        if (!::ScreenToClient(target, &pt)) break;
        const HWND next = ::ChildWindowFromPointEx(target, pt, CWP_SKIPDISABLED | CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
        if (!next || next == target) break;
        target = next;
    }
    return target;
}

bool targetClientPoint(HWND target, POINT& point, bool requireInside) {
    // The thumbnail is measured in physical pixels. Older applications receive
    // mouse messages in their own DPI-dependent client coordinate system.
    if (!::PhysicalToLogicalPointForPerMonitorDPI(target, &point)) return false;
    const auto previous = ::SetThreadDpiAwarenessContext(::GetWindowDpiAwarenessContext(target));
    if (!previous) return false;
    RECT bounds{};
    const bool valid = ::ScreenToClient(target, &point) &&
        (!requireInside || (::GetClientRect(target, &bounds) && ::PtInRect(&bounds, point)));
    ::SetThreadDpiAwarenessContext(previous);
    return valid;
}

WPARAM mouseState(Qt::MouseButtons buttons, Qt::KeyboardModifiers mods) {
    return (buttons.testFlag(Qt::LeftButton) ? MK_LBUTTON : 0)
           | (buttons.testFlag(Qt::RightButton) ? MK_RBUTTON : 0)
           | (buttons.testFlag(Qt::MiddleButton) ? MK_MBUTTON : 0)
           | (mods.testFlag(Qt::ControlModifier) ? MK_CONTROL : 0)
           | (mods.testFlag(Qt::ShiftModifier) ? MK_SHIFT : 0);
}

UINT buttonMessage(Qt::MouseButton button, QEvent::Type type) {
    const bool up = type == QEvent::MouseButtonRelease;
    const bool twice = type == QEvent::MouseButtonDblClick;
    switch (button) {
    case Qt::LeftButton: return up ? WM_LBUTTONUP : twice ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN;
    case Qt::RightButton: return up ? WM_RBUTTONUP : twice ? WM_RBUTTONDBLCLK : WM_RBUTTONDOWN;
    case Qt::MiddleButton: return up ? WM_MBUTTONUP : twice ? WM_MBUTTONDBLCLK : WM_MBUTTONDOWN;
    default: return 0;
    }
}
}

WindowPreview::WindowPreview(HWND source, std::function<bool()> sourceExists)
    : QWidget(nullptr, Qt::Tool | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                       Qt::WindowCloseButtonHint | Qt::WindowStaysOnTopHint),
      m_source(source), m_sourceExists(std::move(sourceExists)) {
    setObjectName("windowPreview");
    setAttribute(Qt::WA_QuitOnClose, false);
    setAttribute(Qt::WA_ShowWithoutActivating);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    m_canvas = new QLabel(this);
    m_canvas->setObjectName("previewCanvas");
    m_canvas->setAccessibleName(tr("Live window preview"));
    m_canvas->setAutoFillBackground(true);
    m_canvas->setBackgroundRole(QPalette::Base);
    m_canvas->setAlignment(Qt::AlignCenter);
    m_canvas->setWordWrap(true);
    m_canvas->setMinimumSize(150, 80);
    m_canvas->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_canvas->setMouseTracking(true);
    m_canvas->installEventFilter(this);
    layout->addWidget(m_canvas, 1);
    auto* tools = new QHBoxLayout;
    m_cropButton = new QPushButton(tr("Crop"), this);
    m_cropButton->setObjectName("previewCrop");
    m_cropButton->setCheckable(true);
    m_reset = new QPushButton(tr("Show full"), this);
    m_reset->setObjectName("previewShowFull");
    m_reset->setEnabled(false);
    tools->addWidget(m_cropButton);
    tools->addWidget(m_reset);
    tools->addStretch();
    layout->addLayout(tools);
    connect(m_cropButton, &QPushButton::toggled, this, [this](bool cropping) {
        releaseButtons();
        m_dragStart.reset();
        m_cropButton->setText(cropping ? tr("Cancel crop") : tr("Crop"));
        updateCursor();
        refresh();
    });
    connect(m_reset, &QPushButton::clicked, this, [this] {
        m_crop = fullWindow;
        m_cropButton->setChecked(false);
        refresh();
    });
    auto* escape = new QShortcut(Qt::Key_Escape, this);
    connect(escape, &QShortcut::activated, this, [this] { m_cropButton->setChecked(false); });
    setInteractive(true);
}

WindowPreview::~WindowPreview() {
    releaseButtons();
    if (m_thumbnail) ::DwmUnregisterThumbnail(m_thumbnail);
}

bool WindowPreview::open() {
    if (!m_sourceExists() || FAILED(::DwmRegisterThumbnail(reinterpret_cast<HWND>(winId()), m_source, &m_thumbnail)))
        return false;
    wchar_t title[256]{};
    ::GetWindowTextW(m_source, title, int(std::size(title)));
    setWindowTitle(*title ? tr("PiP — %1").arg(QString::fromWCharArray(title)) : tr("Picture in Picture"));
    // Use Qt's screen geometry for placement, including on mixed-scale monitors.
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    QScreen* screen = this->screen();
    if (::GetMonitorInfoW(::MonitorFromWindow(m_source, MONITOR_DEFAULTTONEAREST), &monitor)) {
        for (QScreen* candidate : QGuiApplication::screens()) {
            if (candidate->name() == QString::fromWCharArray(monitor.szDevice)) screen = candidate;
        }
    }
    const QRect available = screen->availableGeometry();
    resize(qMin(400, available.width()), qMin(270, available.height()));
    move(available.right() - width() - 16, available.bottom() - height() - 48);
    show();
    layout()->activate();
    refresh();
    return m_live;
}

void WindowPreview::beginCrop() {
    m_cropButton->setChecked(true);
    raise();
    activateWindow();
    m_cropButton->setFocus();
}

void WindowPreview::setInteractive(bool enabled) {
    if (!enabled) releaseButtons();
    m_interactive = enabled;
    updateCursor();
}

void WindowPreview::updateCursor() {
    const bool cropping = m_cropButton->isChecked();
    m_canvas->setCursor(cropping ? Qt::CrossCursor : m_interactive ? Qt::PointingHandCursor : Qt::ArrowCursor);
    m_canvas->setToolTip(cropping ? tr("Drag over the area to keep. Press Esc to cancel.")
                                 : m_interactive ? tr("Click to control the source window, then type normally.") : QString());
}

QRectF WindowPreview::croppedArea(const QRectF& selection) const {
    const QRectF area = selection.normalized().intersected(m_view);
    return {m_crop.x() + (area.x() - m_view.x()) / m_view.width() * m_crop.width(),
            m_crop.y() + (area.y() - m_view.y()) / m_view.height() * m_crop.height(),
            area.width() / m_view.width() * m_crop.width(), area.height() / m_view.height() * m_crop.height()};
}

void WindowPreview::refresh() {
    if (!m_thumbnail || !m_sourceExists()) return;
    SIZE sourceSize{};
    DWORD cloaked = 0;
    const bool available = ::IsWindowVisible(m_source) && !::IsIconic(m_source) &&
        !(SUCCEEDED(::DwmGetWindowAttribute(m_source, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) &&
        SUCCEEDED(::DwmQueryThumbnailSourceSize(m_thumbnail, &sourceSize)) && sourceSize.cx > 0 && sourceSize.cy > 0 &&
        ::GetWindowRect(m_source, &m_sourceFrame);
    if (!available) {
        DWM_THUMBNAIL_PROPERTIES properties{};
        properties.dwFlags = DWM_TNP_VISIBLE;
        if (m_live) ::DwmUpdateThumbnailProperties(m_thumbnail, &properties);
        m_live = false;
        m_dragStart.reset();
        releaseButtons();
        m_canvas->setText(tr("Preview paused. Restore the source window to continue."));
        return;
    }
    RECT visibleFrame{};
    if (SUCCEEDED(::DwmGetWindowAttribute(m_source, DWMWA_EXTENDED_FRAME_BOUNDS, &visibleFrame, sizeof(visibleFrame))) &&
        visibleFrame.right - visibleFrame.left == sourceSize.cx && visibleFrame.bottom - visibleFrame.top == sourceSize.cy)
        m_sourceFrame = visibleFrame;
    if (!m_canvas->text().isEmpty()) m_canvas->clear();
    QSizeF fit(m_crop.width() * sourceSize.cx, m_crop.height() * sourceSize.cy);
    fit.scale(m_canvas->size(), Qt::KeepAspectRatio);
    if (fit.isEmpty()) return;
    m_view = QRectF(QPointF(), fit);
    m_view.moveCenter(QRectF(m_canvas->rect()).center());
    QRectF dest = m_view, source = m_crop;
    if (m_dragStart) {
        const auto selection = QRectF(*m_dragStart, m_dragEnd).normalized().intersected(m_view);
        if (selection.width() >= 2 && selection.height() >= 2) {
            dest = selection;
            source = croppedArea(selection);
        }
    }
    dest.translate(m_canvas->mapTo(this, QPoint()));
    const qreal scale = devicePixelRatioF();
    DWM_THUMBNAIL_PROPERTIES props{};
    props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE;
    props.rcSource = pixels(source, sourceSize.cx, sourceSize.cy);
    // A crop still occupies at least one source pixel after a large source resize.
    props.rcSource.left = std::clamp(props.rcSource.left, 0L, sourceSize.cx - 1);
    props.rcSource.top = std::clamp(props.rcSource.top, 0L, sourceSize.cy - 1);
    props.rcSource.right = std::clamp(props.rcSource.right, props.rcSource.left + 1, sourceSize.cx);
    props.rcSource.bottom = std::clamp(props.rcSource.bottom, props.rcSource.top + 1, sourceSize.cy);
    props.rcDestination = pixels(dest, scale, scale);
    props.fVisible = TRUE;
    if (!m_live || !::EqualRect(&m_lastSource, &props.rcSource) || !::EqualRect(&m_lastDestination, &props.rcDestination)) {
        m_live = SUCCEEDED(::DwmUpdateThumbnailProperties(m_thumbnail, &props));
        m_lastSource = props.rcSource;
        m_lastDestination = props.rcDestination;
    }
    m_reset->setEnabled(m_crop != fullWindow);
}

bool WindowPreview::mapToSource(const QPointF& position, POINT& point) const {
    if (!m_live || !m_sourceExists() || (!m_pressedWindow && !m_view.contains(position))) return false;
    const qreal x = m_crop.x() + (position.x() - m_view.x()) / m_view.width() * m_crop.width();
    const qreal y = m_crop.y() + (position.y() - m_view.y()) / m_view.height() * m_crop.height();
    point = {m_sourceFrame.left + qFloor(x * (m_sourceFrame.right - m_sourceFrame.left)),
             m_sourceFrame.top + qFloor(y * (m_sourceFrame.bottom - m_sourceFrame.top))};
    return true;
}

bool WindowPreview::forwardMouse(QMouseEvent* e) {
    POINT screen{};
    if (!m_interactive || !mapToSource(e->position(), screen)) return false;
    const HWND target = m_pressedWindow ? m_pressedWindow : childWindowAt(m_source, screen);
    if (target != m_source && !::IsChild(m_source, target)) return false;
    POINT client = screen;
    if (!targetClientPoint(target, client, !m_pressedWindow)) return false;
    auto type = e->type();
    if (type == QEvent::MouseButtonDblClick && !(::GetClassLongPtrW(target, GCL_STYLE) & CS_DBLCLKS))
        type = QEvent::MouseButtonPress;
    const UINT message = type == QEvent::MouseMove ? WM_MOUSEMOVE : buttonMessage(e->button(), type);
    if (!message) return false;
    auto buttons = m_pressedButtons;
    if (type == QEvent::MouseButtonRelease && !buttons.testFlag(e->button())) return false;
    if (type != QEvent::MouseMove) buttons.setFlag(e->button(), type != QEvent::MouseButtonRelease);
    m_lastMousePoint = MAKELPARAM(client.x, client.y);
    if (!::PostMessageW(target, message, mouseState(buttons, e->modifiers()), m_lastMousePoint)) return false;
    m_pressedButtons = buttons;
    m_pressedWindow = m_pressedButtons ? target : nullptr;
    // Hand keyboard focus to the real app after the click, preserving keyboard layouts, shortcuts and IME.
    if (e->type() == QEvent::MouseButtonRelease && !m_pressedWindow) ::SetForegroundWindow(m_source);
    return true;
}

void WindowPreview::releaseButtons() {
    if (m_pressedWindow && m_sourceExists() && (m_pressedWindow == m_source || ::IsChild(m_source, m_pressedWindow))) {
        for (auto button : {Qt::LeftButton, Qt::RightButton, Qt::MiddleButton}) {
            if (!m_pressedButtons.testFlag(button)) continue;
            m_pressedButtons.setFlag(button, false);
            ::PostMessageW(m_pressedWindow, buttonMessage(button, QEvent::MouseButtonRelease),
                           mouseState(m_pressedButtons, {}), m_lastMousePoint);
        }
    }
    m_pressedWindow = nullptr;
    m_pressedButtons = {};
}

bool WindowPreview::forwardWheel(QWheelEvent* e) {
    POINT screen{};
    if (!m_interactive || !mapToSource(e->position(), screen)) return false;
    HWND target = childWindowAt(m_source, screen);
    if (!::PhysicalToLogicalPointForPerMonitorDPI(target, &screen)) return false;
    const bool horizontal = e->angleDelta().y() == 0;
    const int delta = horizontal ? e->angleDelta().x() : e->angleDelta().y();
    return delta && ::PostMessageW(target, horizontal ? WM_MOUSEHWHEEL : WM_MOUSEWHEEL,
        MAKEWPARAM(mouseState(e->buttons(), e->modifiers()), delta), MAKELPARAM(screen.x, screen.y));
}

bool WindowPreview::eventFilter(QObject* object, QEvent* event) {
    if (object != m_canvas) return QWidget::eventFilter(object, event);
    if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide) releaseButtons();
    if (event->type() == QEvent::Wheel) return !m_cropButton->isChecked() && forwardWheel(static_cast<QWheelEvent*>(event));
    if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonRelease &&
        event->type() != QEvent::MouseButtonDblClick && event->type() != QEvent::MouseMove) return false;
    auto* e = static_cast<QMouseEvent*>(event);
    if (!m_cropButton->isChecked()) {
        const bool forwarded = forwardMouse(e);
        if (!forwarded && e->type() == QEvent::MouseButtonRelease) releaseButtons();
        return forwarded;
    }
    if (!m_live) return true;
    if (e->type() == QEvent::MouseButtonPress && e->button() == Qt::LeftButton && m_view.contains(e->position())) {
        m_dragStart = m_dragEnd = e->position();
    } else if (m_dragStart) {
        m_dragEnd = e->position();
        if (e->type() == QEvent::MouseButtonRelease && e->button() == Qt::LeftButton) {
            const QRectF selection = QRectF(*m_dragStart, m_dragEnd).normalized().intersected(m_view);
            m_dragStart.reset();
            if (selection.width() >= 5 && selection.height() >= 5) {
                m_crop = croppedArea(selection);
                m_cropButton->setChecked(false);
            }
        }
    }
    refresh();
    return true;
}

void WindowPreview::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    refresh();
}
