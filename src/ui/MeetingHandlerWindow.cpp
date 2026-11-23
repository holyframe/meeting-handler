#include "MeetingHandlerWindow.h"
#include "CaptionPane.h"
#include "CaptionTheme.h"
#include "MeetingControllerPane.h"

#include <QApplication>
#include <QScreen>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>

MeetingHandlerWindow::MeetingHandlerWindow(const Settings& captionSettings, QSettings& layout)
    : sidebarWidth_(captionSettings.sidePanelWidth),
      meetingColumnWidth_(qBound(1, layout.value("meetingColumnWidth", 360).toInt(), QWIDGETSIZE_MAX)) {
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
    columns_->setHandleWidth(caption_theme::splitterThickness);
    auto* sidebar = new QWidget;
    sidebar->setObjectName("captionSidebar");
    sidebar->setMinimumWidth(56);
    columns_->addWidget(sidebar);
    controller_ = new MeetingControllerPane(columns_);
    columns_->addWidget(controller_);
    auto* captions = new CaptionPane(sidebar, columns_);
    columns_->addWidget(captions);
    columns_->setStretchFactor(0, 0);
    columns_->setStretchFactor(1, 0);
    columns_->setStretchFactor(2, 1);
    column->addWidget(columns_, 1);
    auto* log = controller_->logPanel();
    log->setParent(workspace);
    log->setStyleSheet(controller_->styleSheet());
    column->addWidget(log, 1);
    setCentralWidget(workspace);
    const int columnsMinimum = sidebar->minimumWidth() + controller_->minimumWidth()
        + captions->minimumWidth() + 2 * columns_->handleWidth();
    setMinimumWidth(qMax(columnsMinimum, log->minimumSizeHint().width()));
    if (layout.value("meetingsCollapsed").toBool())
        controller_->setMeetingsCollapsed(true);

    connect(columns_, &QSplitter::splitterMoved, this, [this, captions] {
        const auto widths = columns_->sizes();
        sidebarWidth_ = widths.at(0);
        captions->setTargetSidebarWidth(sidebarWidth_);
        if (!controller_->meetingsCollapsed()) meetingColumnWidth_ = widths.at(1);
    });
    connect(controller_, &MeetingControllerPane::meetingsCollapsedChanged, this, [this] {
        setColumnWidths(columns_->sizes().first());
    });
    connect(captions, &CaptionPane::logLine, controller_, &MeetingControllerPane::appendLog);
    connect(captions, &CaptionPane::headerPaletteChanged, controller_, &MeetingControllerPane::setHeaderPalette);
    connect(captions, &CaptionPane::logToggled, controller_, &MeetingControllerPane::setLogVisible);
    connect(controller_, &MeetingControllerPane::logVisibilityChanged, captions,
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
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this, &layout] {
        layout.setValue("geometry", saveGeometry());
        layout.setValue("meetingsCollapsed", controller_->meetingsCollapsed());
        layout.setValue("meetingColumnWidth", meetingColumnWidth_);
        layout.remove("splitter");
        layout.sync();
    });
}

void MeetingHandlerWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (!restoreColumns_) return;
    centralWidget()->layout()->activate();
    setColumnWidths(sidebarWidth_);
    restoreColumns_ = false;
}

void MeetingHandlerWindow::setColumnWidths(int sidebarWidth) {
    const int available = columns_->width() - 2 * columns_->handleWidth();
    const int sidebarMin = columns_->widget(0)->minimumWidth();
    const int meetingsMin = controller_->minimumWidth();
    const int transcriptMin = columns_->widget(2)->minimumWidth();
    const int side = qBound(sidebarMin, sidebarWidth,
                            qMax(sidebarMin, available - meetingsMin - transcriptMin));
    const int middle = controller_->meetingsCollapsed() ? meetingsMin
        : qBound(meetingsMin, meetingColumnWidth_, qMax(meetingsMin, available - side - transcriptMin));
    columns_->setSizes({side, middle, qMax(transcriptMin, available - side - middle)});
}
