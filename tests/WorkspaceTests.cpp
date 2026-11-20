#include "ui/MeetingHandlerWindow.h"
#include "ui/MeetingControllerPane.h"
#include "ui/CaptionPane.h"
#include "ui/CaptionView.h"
#include "ui/SettingsDialog.h"
#include "captions/CaptureEngine.h"
#include "resource.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFileDialog>
#include <QFile>
#include <QIcon>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

namespace {
QRect boundsIn(QWidget* widget, QWidget* parent) {
    return {widget->mapTo(parent, QPoint()), widget->size()};
}

void verifyColumnsFit(QWidget* workspace) {
    auto* columns = workspace->findChild<QSplitter*>("workspaceColumns");
    auto* sidebar = workspace->findChild<QWidget*>("captionSidebar");
    auto* captions = workspace->findChild<CaptionPane*>();
    auto* controller = workspace->findChild<MeetingControllerPane*>();
    QVERIFY(columns && sidebar && captions && controller);
    QCOMPARE(columns->count(), 3);
    QCOMPARE(columns->orientation(), Qt::Horizontal);
    QCOMPARE(columns->widget(0), sidebar);
    QCOMPARE(columns->widget(1), controller);
    QCOMPARE(columns->widget(2), captions);
    QCOMPARE(sidebar->x(), 0);
    QCOMPARE(sidebar->geometry().right() + 1 + columns->handleWidth(), controller->x());
    QCOMPARE(controller->geometry().right() + 1 + columns->handleWidth(), captions->x());
    QCOMPARE(captions->geometry().right() + 1, workspace->width());
    QCOMPARE(sidebar->height(), columns->height());
    QCOMPARE(controller->height(), columns->height());
    QCOMPARE(captions->height(), columns->height());
    auto* view = captions->findChild<CaptionView*>();
    auto* toolbar = captions->findChild<QWidget*>("captionToolbar");
    QVERIFY(view && toolbar);
    QCOMPARE(boundsIn(view, captions).topLeft(), QPoint());
    QCOMPARE(boundsIn(view, captions).right(), captions->rect().right());
    QCOMPARE(boundsIn(toolbar, captions).bottom(), captions->rect().bottom());
    QList<QRect> controls;
    for (auto name : {"sendCaption", "pressEnter", "toggleListening", "saveCaptions", "clearCaptions",
                      "captionPreferences", "toggleActivityLog", "captionFont", "captionSize", "captionSpacing"}) {
        auto* control = toolbar->findChild<QWidget*>(name);
        QVERIFY2(control && control->isVisible(), name);
        const auto rect = boundsIn(control, toolbar);
        if (!toolbar->rect().contains(rect))
            qInfo() << name << rect << "toolbar" << toolbar->rect() << "columns" << columns->sizes() << "caption minimum" << captions->minimumWidth();
        QVERIFY2(toolbar->rect().contains(rect), name);
        for (const auto& other : controls) QVERIFY2(!rect.intersects(other), name);
        controls.append(rect);
    }
}
}

class WorkspaceTests : public QObject {
    Q_OBJECT
private slots:
    void captionDialogSettingsAndSave() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        Settings settings;
        settings.transcriptPath = temp.filePath("capture.txt").toStdWString();
        settings.saveFolder = temp.path().toStdWString();
        settings.hotkeyVirtualKey = 0;
        QWidget owner;
        bool fieldsFound = false;
        const auto editSettings = [&](bool accept) {
            QTimer::singleShot(0, &owner, [&, accept] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) return;
                auto* source = dialog->findChild<QComboBox*>("captionSource");
                auto* theme = dialog->findChild<QComboBox*>("captionTheme");
                auto* hotkey = dialog->findChild<QLineEdit*>("sendHotkey");
                fieldsFound = source && theme && hotkey;
                if (fieldsFound) {
                    source->setCurrentIndex(1);
                    theme->setCurrentIndex(1);
                    QTest::keyClick(hotkey, Qt::Key_F8, Qt::ControlModifier | Qt::AltModifier);
                }
                if (accept) dialog->accept();
                else dialog->reject();
            });
            return ShowSettingsDialog(&owner, settings);
        };
        QVERIFY(!editSettings(false));
        QVERIFY(fieldsFound);
        QCOMPARE(settings.theme, UiTheme::Dark);
        QCOMPARE(settings.hotkeyVirtualKey, 0U);
        QVERIFY(editSettings(true));
        QCOMPARE(settings.theme, UiTheme::Light);
        QCOMPARE(settings.captionSource, CaptionSourcePreference::ChromeLiveCaption);
        QCOMPARE(settings.hotkeyVirtualKey, unsigned(VK_F8));
        QCOMPARE(settings.hotkeyModifiers, unsigned(MOD_CONTROL | MOD_ALT));
        settings.hotkeyVirtualKey = 0;

        QSettings layout(temp.filePath("layout.ini"), QSettings::IniFormat);
        MeetingHandlerWindow window(settings, layout);
        QVERIFY(window.captionsReady());
        window.resize(1200, 640);
        window.show();
        verifyColumnsFit(window.centralWidget());
        auto* captions = window.findChild<CaptionPane*>();
        QVERIFY(captions);
        QCOMPARE(captions->findChild<QSplitter*>("captionRows")->sizes().last(), settings.bottomPanelHeight);
        auto* view = captions->findChild<CaptionView*>();
        auto* save = captions->findChild<QToolButton*>("saveCaptions");
        auto* listen = captions->findChild<QToolButton*>("toggleListening");
        QVERIFY(view && save && listen);
        listen->click();
        auto sendUpdate = [&](const wchar_t* text) {
            QCoreApplication::postEvent(captions, new CaptionUpdate(0, {text}));
        };
        sendUpdate(L"First caption");
        QTRY_VERIFY(!view->Empty());
        bool qtChooser = false;
        const QString path = temp.filePath("saved.txt");
        const auto saveCaptions = [&](bool accept) {
            QTimer::singleShot(0, &window, [&, accept] {
                auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
                if (!dialog) return;
                qtChooser = dialog->testOption(QFileDialog::DontUseNativeDialog);
                if (!accept) { dialog->reject(); return; }
                dialog->selectFile(path);
                sendUpdate(L"Final café caption");
                static_cast<QDialog*>(dialog)->accept();
            });
            save->click();
        };
        saveCaptions(false);
        QVERIFY(qtChooser);
        QCOMPARE(QString::fromStdWString(view->FullText()), QStringLiteral("First caption\r\n"));
        QVERIFY(!QFile::exists(path));
        saveCaptions(true);
        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), QByteArray::fromHex("efbbbf") + QStringLiteral("Final café caption\r\n").toUtf8());
        QVERIFY(view->Empty());
    }

    void threeColumnsShareOneWindow() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings layout(temp.filePath("layout.ini"), QSettings::IniFormat);
        Settings settings;
        settings.transcriptPath = temp.filePath("captions.txt").toStdWString();
        settings.hotkeyVirtualKey = 0;
        settings.sidePanelWidth = 273;
        QPointer<CaptionView> formerView;
        int savedMeetingsWidth = 0;
        {
            MeetingHandlerWindow window(settings, layout);
            QVERIFY(window.captionsReady());
            window.resize(1200, 640);
            window.show();
            window.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto* workspace = window.findChild<QWidget*>("workspace");
            QVERIFY(workspace);
            verifyColumnsFit(workspace);
            QVERIFY(!QIcon(":/icons/meeting-handler.ico").pixmap(16, 16).isNull());
            auto* captions = workspace->findChild<CaptionPane*>();
            auto* columns = workspace->findChild<QSplitter*>("workspaceColumns");
            QCOMPARE(columns->sizes().first(), settings.sidePanelWidth);
            auto* strip = workspace->findChild<QWidget*>("windowDragStrip");
            QCOMPARE(strip->grab(QRect(0, 0, 8, 8)).toImage().pixelColor(1, 1), QColor(32, 32, 32));
            auto* controller = workspace->findChild<MeetingControllerPane*>();
            QVERIFY(controller && !controller->isWindow() && !controller->styleSheet().isEmpty());
            auto* log = window.findChild<QPlainTextEdit*>("appLog");
            auto* logPanel = window.findChild<QWidget*>("appLogPanel");
            QVERIFY(log && logPanel && !logPanel->isVisible());
            QCOMPARE(logPanel->parentWidget(), workspace);
            QVERIFY(log->toPlainText().contains("Ready"));
            auto* collapse = controller->findChild<QPushButton*>("collapseMeetingsButton");
            QVERIFY(collapse);
            QVERIFY(window.windowTitle().contains("No target selected"));
            auto* view = captions->findChild<CaptionView*>();
            formerView = view;
            QCOMPARE(view->viewport()->grab(QRect(0, 0, 8, 8)).toImage().pixelColor(1, 1), QColor(43, 43, 43));
            auto* listen = captions->findChild<QToolButton*>("toggleListening");
            auto* toggleLog = captions->findChild<QToolButton*>("toggleActivityLog");
            auto* send = captions->findChild<QPushButton*>("sendCaption");
            auto* pressEnter = captions->findChild<QCheckBox*>("pressEnter");
            QVERIFY(view && listen && toggleLog && send && pressEnter);
            QCOMPARE(view->window(), &window);
            listen->click();
            QCOMPARE(listen->accessibleName(), "Resume caption listening");

            QCOMPARE(window.findChildren<QPushButton*>("pageNavButton").size(), 0);
            QVERIFY(!window.findChild<QWidget*>("pageStack"));
            toggleLog->click();
            QTRY_VERIFY(logPanel->isVisible());
            QTRY_COMPARE(columns->geometry().bottom() + 1, logPanel->y());
            QCOMPARE(logPanel->width(), workspace->width());
            QVERIFY(view->isVisible());
            toggleLog->click();
            QTRY_VERIFY(!logPanel->isVisible());
            QTRY_COMPARE(columns->geometry().bottom(), workspace->rect().bottom());

            view->setFocus();
            QTest::keyClick(view, Qt::Key_Tab);
            QVERIFY(captions->isAncestorOf(QApplication::focusWidget()) && !view->hasFocus());
            send->setFocus();
            QTest::keyClick(send, Qt::Key_Tab);
            QTRY_COMPARE(QApplication::focusWidget(), pressEnter);
            auto* spacing = captions->findChild<QComboBox*>("captionSpacing");
            QVERIFY(spacing);
            spacing->setFocus();
            for (int i = 0; i < 5 && !controller->isAncestorOf(QApplication::focusWidget()); ++i)
                QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
            QVERIFY(controller->isAncestorOf(QApplication::focusWidget()));

            QCoreApplication::postEvent(captions, new CaptionUpdate(0, {L"We'll review the meeting agenda and then take questions."}));
            QTRY_COMPARE(QString::fromStdWString(view->FullText()), QStringLiteral("We'll review the meeting agenda and then take questions.\r\n"));
            QVERIFY(window.grab().save("three-column-workspace.png"));
            QTest::keyClick(controller, Qt::Key_Return);
            QVERIFY(!log->toPlainText().contains("Select caption text before sending."));
            QTest::mouseDClick(view->viewport(), Qt::LeftButton, {}, QPoint(122, 15));
            QVERIFY(!log->toPlainText().contains("Pick a web tab before sending."));
            QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, QPoint(5, 15));
            QTest::keyClick(view, Qt::Key_Return);
            QVERIFY(log->toPlainText().contains("Pick a web tab before sending."));
            send->click();
            QVERIFY(log->toPlainText().contains("Pick a web tab before sending."));
            QTest::keyClick(view, Qt::Key_Escape);
            ::PostMessageW(reinterpret_cast<HWND>(window.winId()), WM_HOTKEY, HOTKEY_SEND, 0);
            QTRY_VERIFY(log->toPlainText().contains("Select caption text before sending."));

            const auto expandedWidths = columns->sizes();
            const QSize expandedWindow = window.size();
            QTest::mouseClick(collapse, Qt::LeftButton);
            QTRY_VERIFY(controller->meetingsCollapsed());
            QVERIFY(!window.findChild<QWidget*>("pageRoot")->isVisible());
            QVERIFY(collapse->isVisible());
            QCOMPARE(boundsIn(controller->findChild<QWidget*>("collapseBar"), controller).top(), 0);
            QTRY_VERIFY(controller->width() < expandedWidths.at(1));
            QCOMPARE(columns->sizes().first(), expandedWidths.first());
            QCOMPARE(captions->width(), expandedWidths.last() + expandedWidths.at(1) - controller->width());
            QCOMPARE(window.size(), expandedWindow);
            QTest::mouseClick(collapse, Qt::LeftButton);
            QTRY_VERIFY(!controller->meetingsCollapsed());
            QVERIFY(window.findChild<QWidget*>("pageRoot")->isVisible());
            QTRY_COMPARE(columns->sizes(), expandedWidths);
            QCOMPARE(window.size(), expandedWindow);

            auto* preferences = captions->findChild<QToolButton*>("captionPreferences");
            QVERIFY(preferences);
            bool foundSettings = false;
            QTimer::singleShot(0, &window, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) return;
                foundSettings = dialog->parentWidget() == &window && dialog->findChild<QComboBox*>("captionSource")
                    && dialog->findChild<QLineEdit*>("sendHotkey") && dialog->findChild<QLineEdit*>("saveFolder");
                dialog->reject();
            });
            preferences->click();
            QVERIFY(foundSettings);
            QVERIFY(window.isEnabled());

            window.resize(window.minimumWidth(), window.height());
            columns->setSizes({950, 324, 100});
            QCoreApplication::processEvents();
            verifyColumnsFit(workspace);
            QVERIFY(boundsIn(spacing, captions).top() > boundsIn(send, captions).bottom());

            window.resize(expandedWindow);
            const int available = columns->width() - 2 * columns->handleWidth();
            columns->setSizes({settings.sidePanelWidth, 360, available - settings.sidePanelWidth - 360});
            auto* handle = columns->handle(2);
            const QPoint center = handle->rect().center();
            QTest::mousePress(handle, Qt::LeftButton, {}, center);
            QTest::mouseMove(handle, center + QPoint(36, 0));
            QTest::mouseRelease(handle, Qt::LeftButton);
            QTRY_VERIFY(controller->width() > 360);
            savedMeetingsWidth = controller->width();
            collapse->click();

            QTimer::singleShot(0, &window, &QWidget::close);
            QCOMPARE(QApplication::exec(), 0);
            QVERIFY(layout.value("splitter").toByteArray().isEmpty());
            QVERIFY(!layout.value("geometry").toByteArray().isEmpty());
            QVERIFY(layout.value("meetingsCollapsed").toBool());
            QCOMPARE(layout.value("meetingColumnWidth").toInt(), savedMeetingsWidth);
        }
        QVERIFY(formerView.isNull());
        settings.theme = UiTheme::Light;
        MeetingHandlerWindow reopened(settings, layout);
        QVERIFY(reopened.captionsReady());
        auto* workspace = reopened.findChild<QWidget*>("workspace");
        QVERIFY(workspace);
        reopened.show();
        QVERIFY(QTest::qWaitForWindowExposed(&reopened));
        verifyColumnsFit(workspace);
        auto* controller = workspace->findChild<MeetingControllerPane*>();
        QVERIFY(controller->meetingsCollapsed());
        controller->setMeetingsCollapsed(false);
        QTRY_COMPARE(controller->width(), savedMeetingsWidth);
        QCOMPARE(workspace->findChild<QWidget*>("windowDragStrip")->grab(QRect(0, 0, 8, 8)).toImage().pixelColor(1, 1),
                 QColor(Qt::white));
        QCOMPARE(workspace->findChild<CaptionView*>()->viewport()->grab(QRect(0, 0, 8, 8)).toImage().pixelColor(1, 1),
                 QColor(Qt::white));
        reopened.showMaximized();
        QTRY_VERIFY(reopened.isMaximized());
        QCoreApplication::processEvents();
        verifyColumnsFit(workspace);
    }
};

int main(int argc, char** argv) {
    if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    int result;
    {
        QApplication app(argc, argv);
        WorkspaceTests tests;
        result = QTest::qExec(&tests, argc, argv);
    }
    ::CoUninitialize();
    return result;
}
#include "WorkspaceTests.moc"
