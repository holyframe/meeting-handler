#include "MeetingHandlerWindow.h"
#include "CaptionPane.h"
#include "AppTheme.h"
#include "MeetingControllerPane.h"

#include <QApplication>
#include <QPainter>
#include <QScreen>
#include <QSettings>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindowStateChangeEvent>

namespace {
constexpr int collapsedDividerWidth = 9;
constexpr int dividerSpaceFreedOnCollapse = 2 * app_theme::splitterThickness - collapsedDividerWidth;

class ExpandMeetingsButton final : public QToolButton {
public:
    explicit ExpandMeetingsButton(QSplitterHandle* handle) : QToolButton(handle) {
        setObjectName("expandMeetingsButton");
        setAccessibleName(tr("Expand meetings"));
        setToolTip(tr("Expand meetings"));
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover);
        setFixedSize(collapsedDividerWidth, 72);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        if (hasFocus() || underMouse() || isDown())
            painter.fillRect(rect(), palette().mid());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().buttonText());
        const QPointF center = QRectF(rect()).center();
        for (int dot = -2; dot <= 2; ++dot)
            painter.drawEllipse(center + QPointF(0, 9 * dot), 1.8, 1.8);
    }
};
}

MeetingHandlerWindow::MeetingHandlerWindow(const Settings& captionSettings, QSettings& layout)
    : sidebarWidth_(captionSettings.sidePanelWidth),
      meetingColumnWidth_(qBound(1, layout.value("meetingColumnWidth", 360).toInt(), QWIDGETSIZE_MAX)),
      pendingNormalSize_(layout.value("pendingNormalSize").toSize()) {
    setWindowFlag(Qt::FramelessWindowHint, captionSettings.compactView);
    setWindowFlag(Qt::WindowStaysOnTopHint, captionSettings.compactView || captionSettings.alwaysOnTop);

    auto* workspace = new QWidget(this);
    workspace->setObjectName("workspace");
    auto* column = new QVBoxLayout(workspace);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    columns_ = new QSplitter(Qt::Horizontal, workspace);
    columns_->setObjectName("workspaceColumns");
    columns_->setChildrenCollapsible(false);
    columns_->setHandleWidth(app_theme::splitterThickness);
    auto* sidebar = new QWidget;
    sidebar->setObjectName("captionSidebar");
    sidebar->setMinimumWidth(56);
    columns_->addWidget(sidebar);
    auto* captions = new CaptionPane(sidebar, columns_);
    columns_->addWidget(captions);
    controller_ = new MeetingControllerPane(columns_, &captions->windowManager());
    columns_->insertWidget(1, controller_);
    // Qt keeps this handle between the sidebar and transcript when Meetings is hidden.
    auto* divider = columns_->handle(2);
    auto* gripLayout = new QVBoxLayout(divider);
    gripLayout->setContentsMargins(0, 0, 0, 0);
    auto* expand = new ExpandMeetingsButton(divider);
    gripLayout->addWidget(expand, 0, Qt::AlignCenter);
    expand->hide();
    divider->setBackgroundRole(QPalette::AlternateBase);
    divider->setAutoFillBackground(true);
    connect(expand, &QToolButton::clicked, controller_, [this] {
        controller_->setMeetingsCollapsed(false);
        controller_->setFocus(Qt::OtherFocusReason);
    });
    connect(controller_, &MeetingControllerPane::meetingsCollapsedChanged, this, [this, expand](bool collapsed) {
        expand->setVisible(collapsed);
        columns_->setHandleWidth(collapsed ? collapsedDividerWidth : app_theme::splitterThickness);
        if (collapsed && expand->isVisible()) expand->setFocus(Qt::OtherFocusReason);
    });
    columns_->setStretchFactor(0, 0);
    columns_->setStretchFactor(1, 0);
    columns_->setStretchFactor(2, 1);
    column->addWidget(columns_, 1);
    setCentralWidget(workspace);
    if (layout.value("meetingsCollapsed").toBool())
        controller_->setMeetingsCollapsed(true);
    updateMinimumWidth();

    const auto rememberColumns = [this, captions] {
        const auto widths = columns_->sizes();
        if (!controller_->meetingsCollapsed()) meetingColumnWidth_ = widths.at(1);
        sidebarWidth_ = qMax(columns_->widget(0)->minimumWidth(), widths.at(0) - maximizedSidebarExtra());
        captions->setTargetSidebarWidth(sidebarWidth_);
    };
    connect(columns_, &QSplitter::splitterMoved, this, rememberColumns);
    connect(controller_, &MeetingControllerPane::meetingsCollapseAboutToChange, this, [this, rememberColumns] {
        if (restoreColumns_) return;
        widthsBeforeToggle_ = columns_->sizes();
        rememberColumns();
    });
    connect(controller_, &MeetingControllerPane::meetingsCollapsedChanged, this, &MeetingHandlerWindow::applyMeetingsToggle);
    connect(captions, &CaptionPane::logLine, controller_, &MeetingControllerPane::appendLog);
    connect(captions, &CaptionPane::logToggled, controller_, &MeetingControllerPane::setLogVisible);
    connect(controller_, &MeetingControllerPane::logVisibilityChanged, captions,
            &CaptionPane::setLogVisible);
    connect(captions, &QWidget::windowTitleChanged, this, [this](const QString& title) {
        setWindowTitle(QStringLiteral("Meeting Handler — ") + title);
    });

    const QSize available = screen()->availableGeometry().size() - QSize(16, 48);
    resize(QSize(1200, 900).boundedTo(available));
    const bool restored = restoreGeometry(layout.value("geometry").toByteArray());
    if (restored && controller_->meetingsCollapsed()) {
        // Older layouts saved a 48-pixel rail between two six-pixel splitter handles.
        const int oldSpan = layout.value("collapsedDividerWidth", 48 + 2 * 6).toInt();
        const int newSpan = columns_->handleWidth();
        if (oldSpan > 0 && oldSpan <= QWIDGETSIZE_MAX && oldSpan != newSpan) {
            if (pendingNormalSize_.isEmpty())
                pendingNormalSize_ = isMaximized() || isFullScreen() ? normalGeometry().size() : size();
            if (!pendingNormalSize_.isEmpty())
                pendingNormalSize_.setWidth(qMax(minimumWidth(), pendingNormalSize_.width() + newSpan - oldSpan));
        }
    }
    captionsReady_ = captions->initialize(captionSettings);
    if (!captionsReady_) return;

    // Old caption settings store physical screen coordinates. Reuse a valid
    // location on the first combined launch, including secondary monitors.
    if (!restored && (captionSettings.windowX != -1 || captionSettings.windowY != -1)) {
        const POINT position{captionSettings.windowX, captionSettings.windowY};
        if (::MonitorFromPoint(position, MONITOR_DEFAULTTONULL))
            ::SetWindowPos(reinterpret_cast<HWND>(winId()), nullptr, position.x, position.y,
                           0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this, &layout] {
        layout.setValue("geometry", saveGeometry());
        layout.setValue("meetingsCollapsed", controller_->meetingsCollapsed());
        layout.setValue("meetingColumnWidth", meetingColumnWidth_);
        if (controller_->meetingsCollapsed()) layout.setValue("collapsedDividerWidth", columns_->handleWidth());
        else layout.remove("collapsedDividerWidth");
        if (pendingNormalSize_.isEmpty()) layout.remove("pendingNormalSize");
        else layout.setValue("pendingNormalSize", pendingNormalSize_);
        layout.remove("splitter");
        layout.sync();
    });
}

void MeetingHandlerWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (!restoreColumns_) return;
    restoreColumns_ = false;
    restoreColumnWidths();
}

int MeetingHandlerWindow::splitterHandlesWidth() const {
    return (controller_->meetingsCollapsed() ? 1 : 2) * columns_->handleWidth();
}

void MeetingHandlerWindow::updateMinimumWidth() {
    const int meetingsMin = controller_->meetingsCollapsed() ? 0 : controller_->minimumWidth();
    const int columnsMinimum = columns_->widget(0)->minimumWidth() + meetingsMin
        + columns_->widget(2)->minimumWidth() + splitterHandlesWidth();
    setMinimumWidth(columnsMinimum);
}

int MeetingHandlerWindow::maximizedSidebarExtra() const {
    if (!(isMaximized() || isFullScreen()) || !controller_->meetingsCollapsed()) return 0;
    return meetingColumnWidth_ + dividerSpaceFreedOnCollapse;
}

void MeetingHandlerWindow::applyMeetingsToggle() {
    // Settle the native handles before assigning widths for the new set of visible panes.
    columns_->refresh();
    updateMinimumWidth();
    if (widthsBeforeToggle_.isEmpty()) return;
    const bool collapsed = controller_->meetingsCollapsed();
    const int middle = collapsed ? 0 : qMax(controller_->minimumWidth(), meetingColumnWidth_);
    const int transcript = widthsBeforeToggle_.at(2);
    if (isMaximized() || isFullScreen()) {
        // During a Windows restore the widget can still report its maximized size.
        // Apply the delta to the saved normal geometry while it is available.
        if (pendingNormalSize_.isEmpty()) pendingNormalSize_ = normalGeometry().size();
        if (!pendingNormalSize_.isEmpty()) {
            const int widthChange = (collapsed ? widthsBeforeToggle_.at(1) : middle) + dividerSpaceFreedOnCollapse;
            pendingNormalSize_.rwidth() += collapsed ? -widthChange : widthChange;
        }
    } else {
        resize(widthsBeforeToggle_.first() + middle + transcript + splitterHandlesWidth(), height());
    }
    centralWidget()->layout()->activate();
    setColumnWidths(columns_->width() - splitterHandlesWidth() - middle - transcript);
    widthsBeforeToggle_.clear();
}

void MeetingHandlerWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange || restoreColumns_) return;
    const auto fixedStates = Qt::WindowMaximized | Qt::WindowFullScreen;
    const auto oldState = static_cast<QWindowStateChangeEvent*>(event)->oldState();
    if ((oldState & fixedStates) != (windowState() & fixedStates))
        QTimer::singleShot(0, this, &MeetingHandlerWindow::restoreColumnWidths);
}

void MeetingHandlerWindow::restoreColumnWidths() {
    if (isMinimized()) return;
    if (!isMaximized() && !isFullScreen() && !pendingNormalSize_.isEmpty()) {
        resize(pendingNormalSize_);
        pendingNormalSize_ = {};
    }
    centralWidget()->layout()->activate();
    setColumnWidths(sidebarWidth_ + maximizedSidebarExtra());
}

void MeetingHandlerWindow::setColumnWidths(int sidebarWidth) {
    const int available = columns_->width() - splitterHandlesWidth();
    const int sidebarMin = columns_->widget(0)->minimumWidth();
    const int meetingsMin = controller_->meetingsCollapsed() ? 0 : controller_->minimumWidth();
    const int transcriptMin = columns_->widget(2)->minimumWidth();
    const int side = qBound(sidebarMin, sidebarWidth,
                            qMax(sidebarMin, available - meetingsMin - transcriptMin));
    const int middle = controller_->meetingsCollapsed() ? meetingsMin
        : qBound(meetingsMin, meetingColumnWidth_, qMax(meetingsMin, available - side - transcriptMin));
    columns_->setSizes({side, middle, qMax(transcriptMin, available - side - middle)});
}
