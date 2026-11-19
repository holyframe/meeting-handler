#include "MeetingHandlerWindow.h"
#include "CaptionPane.h"
#include "MeetingControllerPane.h"

#include <QApplication>
#include <QScreen>
#include <QSettings>
#include <QVBoxLayout>

MeetingHandlerWindow::MeetingHandlerWindow(const Settings& captionSettings, QSettings& layout) {
    setWindowFlag(Qt::FramelessWindowHint, captionSettings.compactView);
    setWindowFlag(Qt::WindowStaysOnTopHint, captionSettings.compactView || captionSettings.alwaysOnTop);

    setMinimumWidth(1120);
    auto* workspace = new QWidget(this);
    workspace->setObjectName("workspace");
    auto* column = new QVBoxLayout(workspace);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    auto* captions = new CaptionPane(workspace);
    auto* controller = new MeetingControllerPane(workspace);
    auto* log = controller->logPanel();
    log->setParent(workspace);
    log->setStyleSheet(controller->styleSheet());
    column->addWidget(captions, 1);
    column->addWidget(log, 1);
    column->addWidget(controller, 0);
    setCentralWidget(workspace);
    if (layout.value("meetingsCollapsed").toBool())
        controller->setMeetingsCollapsed(true);

    connect(controller, &MeetingControllerPane::meetingsCollapsedChanged, this,
            [this](bool collapsed, int contentHeight) {
                if (!isVisible() || isMaximized() || isFullScreen() || contentHeight <= 0)
                    return;
                const int next = height() + (collapsed ? -contentHeight : contentHeight);
                resize(width(), qMax(minimumHeight(), next));
            });
    connect(captions, &CaptionPane::logLine, controller, &MeetingControllerPane::appendLog);
    connect(captions, &CaptionPane::logToggled, controller, &MeetingControllerPane::setLogVisible);
    connect(controller, &MeetingControllerPane::logVisibilityChanged, captions,
            &CaptionPane::setLogVisible);
    connect(captions, &QWidget::windowTitleChanged, this, [this](const QString& title) {
        setWindowTitle(QStringLiteral("Meeting Handler — ") + title);
    });

    const QSize available = screen()->availableGeometry().size() - QSize(16, 48);
    resize(QSize(1200, 900).boundedTo(available));
    const bool restored = restoreGeometry(layout.value("geometry").toByteArray());
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
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this, controller, &layout] {
        layout.setValue("geometry", saveGeometry());
        layout.setValue("meetingsCollapsed", controller->meetingsCollapsed());
        layout.remove("splitter");
        layout.sync();
    });
}
