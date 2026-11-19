#include "MainWindow.h"
#include "MeetingBatch.h"

#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
#include <cstring>

int main(int argc, char* argv[])
{
    if (argc == 2 && std::strcmp(argv[1], "--meeting-batch-worker") == 0)
    {
        QCoreApplication app(argc, argv);
        return MeetingBatch::runWorker();
    }
    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName("MeetingController");
    QCoreApplication::setApplicationName("MeetingController");
    QCoreApplication::setApplicationVersion("0.4.0");
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.ico")));

    MainWindow window;
    window.show();

    return app.exec();
}
