#include "MeetingHandlerWindow.h"
#include "CaptionPane.h"
#include "MeetingControllerPane.h"

#include <QApplication>
#include <QScreen>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>

MeetingHandlerWindow::MeetingHandlerWindow(const Settings& captionSettings, QSettings& layout) {
    setWindowFlag(Qt::FramelessWindowHint, captionSettings.compactView);
    setWindowFlag(Qt::WindowStaysOnTopHint, captionSettings.compactView || captionSettings.alwaysOnTop);

    setMinimumWidth(1120);
    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->setObjectName("workspaceSplitter");
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(6);
    splitter->setStyleSheet("QSplitter#workspaceSplitter::handle { background: #344459; }"
                            "QSplitter#workspaceSplitter::handle:hover { background: #5A7899; }");
    auto* captions = new CaptionPane(splitter);
    splitter->addWidget(captions);
    auto* controller = new MeetingControllerPane(splitter);
    splitter->addWidget(controller);
    setCentralWidget(splitter);

    auto* status = statusBar();
    status->setStyleSheet("QStatusBar { color: #91A5BC; background: #0A1320; border-top: 1px solid #26364A; }");
    status->showMessage(controller->statusMessage());
    connect(controller, &MeetingControllerPane::statusMessageChanged, status,
            [status](const QString& message) { status->showMessage(message); });
    connect(captions, &QWidget::windowTitleChanged, this, [this](const QString& title) {
        setWindowTitle(QStringLiteral("Meeting Handler — ") + title);
    });

    const QSize available = screen()->availableGeometry().size() - QSize(16, 48);
    resize(QSize(1200, 900).boundedTo(available));
    const bool restored = restoreGeometry(layout.value("geometry").toByteArray());
    if (!splitter->restoreState(layout.value("splitter").toByteArray()))
        splitter->setSizes({450, 450});
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
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this, splitter, &layout] {
        layout.setValue("geometry", saveGeometry());
        layout.setValue("splitter", splitter->saveState());
        layout.sync();
    });
}
