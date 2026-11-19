#include "MainWindow.h"
#include "MeetingBatch.h"
#include "Settings.h"
#include "Workspace.h"

#include <QApplication>
#include <QIcon>
#include <QMessageBox>
#include <QSettings>

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <cstring>

namespace {
int runWindow(int& argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("MeetingController");
    QCoreApplication::setApplicationName("MeetingHandler");
    QCoreApplication::setApplicationVersion("0.4.0");
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.ico")));

    Settings captions;
    captions.Load();
    QSettings layout(QCoreApplication::applicationDirPath() + "/MeetingHandler.ini", QSettings::IniFormat);
    MainWindow window;
    if (!installWorkspace(window, captions, layout)) {
        QMessageBox::critical(nullptr, "Meeting Handler", "Could not create the live caption pane.");
        return 1;
    }
    window.show();
    return app.exec();
}
}

int main(int argc, char** argv) {
    // Batch jobs relaunch this same executable with pipes and no UI. They
    // initialize their own MTA, and must never start a second caption session.
    if (argc == 2 && std::strcmp(argv[1], "--meeting-batch-worker") == 0) {
        QCoreApplication app(argc, argv);
        return MeetingBatch::runWorker();
    }
    if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    ::InitCommonControlsEx(&controls);
    const int result = runWindow(argc, argv);
    ::CoUninitialize();
    return result;
}
