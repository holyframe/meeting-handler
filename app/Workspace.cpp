#include "Workspace.h"
#include "CaptionHost.h"
#include "MainWindow.h"

#include <QApplication>
#include <QScreen>
#include <QSettings>
#include <QSplitter>

bool installWorkspace(MainWindow& window, const Settings& captionSettings, QSettings& layout) {
    window.setWindowFlag(Qt::FramelessWindowHint, captionSettings.compactView);
    window.setWindowFlag(Qt::WindowStaysOnTopHint, captionSettings.compactView || captionSettings.alwaysOnTop);

    // The existing QMainWindow still owns its status bar, controller actions,
    // workers, and dialogs. Only the placement of its central widget changes.
    QWidget* controller = window.takeCentralWidget();
    auto* splitter = new QSplitter(Qt::Vertical, &window);
    splitter->setObjectName("workspaceSplitter");
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(6);
    splitter->setStyleSheet("QSplitter#workspaceSplitter::handle { background: #344459; }"
                            "QSplitter#workspaceSplitter::handle:hover { background: #5A7899; }");
    auto* captions = new CaptionHost(splitter);
    splitter->addWidget(captions);
    splitter->addWidget(controller);
    window.setCentralWidget(splitter);

    window.setMinimumHeight(0);
    const QSize available = window.screen()->availableGeometry().size() - QSize(16, 48);
    window.resize(QSize(1200, 900).boundedTo(available));
    const bool restored = window.restoreGeometry(layout.value("geometry").toByteArray());
    if (!splitter->restoreState(layout.value("splitter").toByteArray()))
        splitter->setSizes({450, 450});
    if (!captions->initialize(captionSettings)) return false;

    // Old caption settings store physical screen coordinates. Reuse a valid
    // location on the first combined launch, including secondary monitors.
    if (!restored && (captionSettings.windowX != -1 || captionSettings.windowY != -1)) {
        const POINT position{captionSettings.windowX, captionSettings.windowY};
        if (::MonitorFromPoint(position, MONITOR_DEFAULTTONULL))
            ::SetWindowPos(reinterpret_cast<HWND>(window.winId()), nullptr, position.x, position.y,
                           0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, &window, [&window, splitter, &layout] {
        layout.setValue("geometry", window.saveGeometry());
        layout.setValue("splitter", splitter->saveState());
        layout.sync();
    });
    return true;
}
