#include "ui/MeetingHandlerWindow.h"
#include "ui/MeetingControllerPane.h"
#include "ui/CaptionPane.h"
#include "captions/CaptureEngine.h"
#include "captions/Settings.h"
#include "resource.h"

#include <QApplication>
#include <QIcon>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <commctrl.h>

namespace {
QString windowText(HWND window) {
    wchar_t text[512]{};
    ::GetWindowTextW(window, text, static_cast<int>(std::size(text)));
    return QString::fromWCharArray(text);
}

QRect windowBounds(HWND window) {
    RECT rect{};
    if (!::GetWindowRect(window, &rect)) return {};
    return QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
}

void verifyCaptionFits(QSplitter* split) {
    auto* captions = qobject_cast<CaptionPane*>(split->widget(0));
    QVERIFY(captions && !captions->isWindow() && captions->parentWidget() == split);
    const HWND pane = reinterpret_cast<HWND>(captions->winId());
    const QRect slot = windowBounds(pane);
    QVERIFY(!slot.isEmpty());
    QCOMPARE(captions->width(), split->contentsRect().width());
    QCOMPARE(captions->geometry().bottom() + 1, split->handle(1)->geometry().top());
    QVERIFY(qAbs(slot.width() - captions->width() * captions->devicePixelRatioF()) <= 1);
    QVERIFY(qAbs(slot.height() - captions->height() * captions->devicePixelRatioF()) <= 1);
    const HWND view = ::GetDlgItem(pane, IDC_CAPTION_VIEW);
    QVERIFY(view && ::GetParent(view) == pane);
    QCOMPARE(windowBounds(view).right(), slot.right());
    QCOMPARE(windowBounds(::GetDlgItem(pane, IDC_STATUSBAR)).bottomRight(), slot.bottomRight());
    QVERIFY(windowBounds(::GetDlgItem(pane, IDC_BTN_SETTINGS)).right() <
            windowBounds(::GetDlgItem(pane, IDC_CBO_FONT)).left());
}

HWND dialogOwner = nullptr;
bool foundSettings = false;
void CALLBACK dismissCaptionSettings(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND dialog = ::GetLastActivePopup(dialogOwner);
    if (!dialog || dialog == dialogOwner || !::GetDlgItem(dialog, IDC_CBO_SOURCE)) return;
    foundSettings = ::GetWindow(dialog, GW_OWNER) == dialogOwner && ::GetDlgItem(dialog, IDC_EDT_HOTKEY);
    ::KillTimer(nullptr, timer);
    ::PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
}
}

class WorkspaceTests : public QObject {
    Q_OBJECT
private slots:
    void stackedPanelsShareOneWindow() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QSettings layout(temp.filePath("layout.ini"), QSettings::IniFormat);
        Settings settings;
        settings.transcriptPath = temp.filePath("captions.txt").toStdWString();
        settings.hotkeyVirtualKey = 0;
        settings.sidePanelWidth = 273;
        HWND native = nullptr;
        QByteArray savedSplit;
        {
            MeetingHandlerWindow window(settings, layout);
            QVERIFY(window.captionsReady());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto* split = window.findChild<QSplitter*>("workspaceSplitter");
            QVERIFY(split && split->count() == 2 && split->orientation() == Qt::Vertical);
            verifyCaptionFits(split);
            QVERIFY(!QIcon(":/icons/meeting-handler.ico").pixmap(16, 16).isNull());
            QCOMPARE(split->widget(0)->grab(QRect(0, 0, 8, 8)).toImage().pixelColor(1, 1),
                     QColor(32, 32, 32));
            auto* controller = qobject_cast<MeetingControllerPane*>(split->widget(1));
            QVERIFY(controller && !controller->isWindow() && !controller->styleSheet().isEmpty());
            QVERIFY(window.statusBar()->currentMessage().startsWith("Ready"));
            const HWND frame = reinterpret_cast<HWND>(window.winId());
            native = reinterpret_cast<HWND>(split->widget(0)->winId());
            QVERIFY(native && ::GetAncestor(native, GA_ROOT) == frame);
            QVERIFY((::GetWindowLongPtrW(native, GWL_STYLE) & WS_CHILD) != 0);
            QVERIFY(window.windowTitle().contains("No target selected"));
            const HWND view = ::GetDlgItem(native, IDC_CAPTION_VIEW);
            const HWND listen = ::GetDlgItem(native, IDC_BTN_LISTEN);
            const HWND status = ::GetDlgItem(native, IDC_STATUSBAR);
            QVERIFY(view && listen && status);
            ::SendMessageW(listen, BM_CLICK, 0, 0);
            QCOMPARE(windowText(listen), "Resume caption listening");

            split->setSizes({220, 600});
            QCoreApplication::processEvents();
            verifyCaptionFits(split);
            QVERIFY(split->widget(0)->geometry().bottom() < split->widget(1)->geometry().top());

            const auto navigation = window.findChildren<QPushButton*>("pageNavButton");
            auto* pages = window.findChild<QStackedWidget*>("pageStack");
            QVERIFY(pages && navigation.size() == 4);
            for (qsizetype i = 0; i < navigation.size(); ++i) {
                navigation[i]->click();
                QCOMPARE(pages->currentIndex(), static_cast<int>(i));
                QCOMPARE(window.statusBar()->currentMessage(), controller->statusMessage());
                QVERIFY(::IsWindowVisible(view));
            }
            navigation.front()->click();

            // Exercise the production dispatcher with queued Windows keyboard
            // messages, without sending global input or contacting a browser.
            ::SetFocus(view);
            ::PostMessageW(view, WM_KEYDOWN, VK_TAB, 0);
            QTRY_VERIFY(::GetFocus() != view && ::IsChild(native, ::GetFocus()));
            ::SetFocus(::GetDlgItem(native, IDC_BTN_SEND));
            ::PostMessageW(::GetFocus(), WM_KEYDOWN, VK_TAB, 0);
            QTRY_COMPARE(::GetFocus(), ::GetDlgItem(native, IDC_CHK_PRESS_ENTER));
            const HWND first = ::GetNextDlgTabItem(native, nullptr, FALSE);
            const HWND last = ::GetNextDlgTabItem(native, first, TRUE);
            QVERIFY(first && last && first != last);
            ::SetFocus(last);
            ::PostMessageW(last, WM_KEYDOWN, VK_TAB, 0);
            QTRY_VERIFY(split->widget(1)->isAncestorOf(QApplication::focusWidget()));
            QVERIFY(!::IsChild(native, ::GetFocus()));
            auto* update = new CaptionUpdate{0, {L"Combined caption test"}};
            ::SendMessageW(native, WM_APP_CAPTION_UPDATE, 0, reinterpret_cast<LPARAM>(update));
            ::SendMessageW(view, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(5, 15));
            ::SendMessageW(view, WM_LBUTTONUP, 0, MAKELPARAM(5, 15));
            ::SendMessageW(::GetDlgItem(native, IDC_BTN_SEND), BM_CLICK, 0, 0);
            QCOMPARE(windowText(status), "Pick a web tab before sending.");
            ::PostMessageW(view, WM_KEYDOWN, VK_ESCAPE, 0);
            ::PostMessageW(native, WM_HOTKEY, HOTKEY_SEND, 0);
            QTRY_COMPARE(windowText(status), "Select caption text before sending.");

            dialogOwner = frame;
            QVERIFY(::SetTimer(nullptr, 0, 25, dismissCaptionSettings) != 0);
            ::SendMessageW(::GetDlgItem(native, IDC_BTN_SETTINGS), BM_CLICK, 0, 0);
            QVERIFY(foundSettings);
            QVERIFY(::IsWindowEnabled(frame));

            savedSplit = split->saveState();
            QTimer::singleShot(0, &window, &QWidget::close);
            QCOMPARE(QApplication::exec(), 0);
            QCOMPARE(layout.value("splitter").toByteArray(), savedSplit);
            QVERIFY(!layout.value("geometry").toByteArray().isEmpty());
        }
        QVERIFY(!::IsWindow(native));
        settings.theme = UiTheme::Light;
        MeetingHandlerWindow reopened(settings, layout);
        QVERIFY(reopened.captionsReady());
        auto* split = reopened.findChild<QSplitter*>("workspaceSplitter");
        QVERIFY(split);
        QCOMPARE(split->saveState(), savedSplit);
        reopened.show();
        QVERIFY(QTest::qWaitForWindowExposed(&reopened));
        verifyCaptionFits(split);
        QCOMPARE(split->widget(0)->grab(QRect(0, 0, 8, 8)).toImage().pixelColor(1, 1),
                 QColor(Qt::white));
        reopened.showMaximized();
        QTRY_VERIFY(reopened.isMaximized());
        QCoreApplication::processEvents();
        verifyCaptionFits(split);
    }
};

int main(int argc, char** argv) {
    if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    ::InitCommonControlsEx(&controls);
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
