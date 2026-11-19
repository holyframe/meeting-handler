#include "MainWindow.h"
#include "Workspace.h"
#include "CaptureEngine.h"
#include "Settings.h"
#include "resource.h"

#include <QApplication>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
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
    QWidget* host = split->widget(0);
    const HWND parent = reinterpret_cast<HWND>(host->winId());
    const HWND pane = ::FindWindowExW(parent, nullptr, L"LiveCaptionViewMain", nullptr);
    const QRect slot = windowBounds(parent);
    QVERIFY(pane && !slot.isEmpty());
    QCOMPARE(host->width(), split->contentsRect().width());
    QCOMPARE(host->geometry().bottom() + 1, split->handle(1)->geometry().top());
    QVERIFY(qAbs(slot.width() - host->width() * host->devicePixelRatioF()) <= 1);
    QVERIFY(qAbs(slot.height() - host->height() * host->devicePixelRatioF()) <= 1);
    QCOMPARE(windowBounds(pane), slot);
    QCOMPARE(windowBounds(::GetDlgItem(pane, IDC_CAPTION_VIEW)).right(), slot.right());
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
            MainWindow window;
            QVERIFY(installWorkspace(window, settings, layout));
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto* split = window.findChild<QSplitter*>("workspaceSplitter");
            QVERIFY(split && split->count() == 2 && split->orientation() == Qt::Vertical);
            verifyCaptionFits(split);
            QCOMPARE(split->widget(0)->objectName(), "liveCaptionHost");
            QCOMPARE(split->widget(1)->objectName(), "appRoot");
            const HWND frame = reinterpret_cast<HWND>(window.winId());
            const HWND host = reinterpret_cast<HWND>(split->widget(0)->winId());
            native = ::FindWindowExW(host, nullptr, L"LiveCaptionViewMain", nullptr);
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
        MainWindow reopened;
        QVERIFY(installWorkspace(reopened, settings, layout));
        auto* split = reopened.findChild<QSplitter*>("workspaceSplitter");
        QVERIFY(split);
        QCOMPARE(split->saveState(), savedSplit);
        reopened.show();
        QVERIFY(QTest::qWaitForWindowExposed(&reopened));
        verifyCaptionFits(split);
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
