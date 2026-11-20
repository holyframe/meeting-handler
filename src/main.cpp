#include "ui/MeetingHandlerWindow.h"
#include "meetings/MeetingBatch.h"
#include "captions/Settings.h"

#include <QApplication>
#include <QIcon>
#include <QMessageBox>
#include <QSettings>

#include <windows.h>
#include <objbase.h>
#include <cstring>

namespace {
int runWindow(int& argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("MeetingController");
    QCoreApplication::setApplicationName("MeetingHandler");
    QCoreApplication::setApplicationVersion(MEETING_HANDLER_VERSION);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/meeting-handler.ico")));

    Settings captions;
    captions.Load();
    QSettings layout(QCoreApplication::applicationDirPath() + "/MeetingHandler.ini", QSettings::IniFormat);
    MeetingHandlerWindow window(captions, layout);
    if (!window.captionsReady()) {
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
    const int result = runWindow(argc, argv);
    ::CoUninitialize();
    return result;
}
