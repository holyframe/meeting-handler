#include "ui/MeetingHandlerWindow.h"
#include "ui/MeetingControllerPane.h"
#include "ui/CaptionPane.h"
#include "captions/CaptureEngine.h"
#include "captions/Settings.h"
#include "resource.h"

#include <QApplication>
#include <QIcon>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QWidget>
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

void verifyCaptionFits(QWidget* workspace) {
    auto* captions = workspace->findChild<CaptionPane*>();
    auto* controller = workspace->findChild<MeetingControllerPane*>();
    QVERIFY(captions && controller);
    QVERIFY(!captions->isWindow() && captions->parentWidget() == workspace);
    QVERIFY(controller->parentWidget() == workspace);
    const HWND pane = reinterpret_cast<HWND>(captions->winId());
    const QRect slot = windowBounds(pane);
    QVERIFY(!slot.isEmpty());
    QCOMPARE(captions->width(), workspace->contentsRect().width());
    QCOMPARE(captions->geometry().bottom() + 1, controller->geometry().top());
    QVERIFY(qAbs(slot.width() - captions->width() * captions->devicePixelRatioF()) <= 1);
    QVERIFY(qAbs(slot.height() - captions->height() * captions->devicePixelRatioF()) <= 1);
    const HWND view = ::GetDlgItem(pane, IDC_CAPTION_VIEW);
    QVERIFY(view && ::GetParent(view) == pane);
    QCOMPARE(windowBounds(view).right(), slot.right());
    QCOMPARE(windowBounds(::GetDlgItem(pane, IDC_BOTTOM_PANEL)).bottom(), slot.bottom());
    const HWND settingsButton = ::GetDlgItem(pane, IDC_BTN_SETTINGS);
    const HWND logButton = ::GetDlgItem(pane, IDC_BTN_LOG);
    QVERIFY(settingsButton && logButton);
    QVERIFY(windowBounds(settingsButton).right() <= windowBounds(logButton).left());
    QVERIFY(windowBounds(logButton).right() < windowBounds(::GetDlgItem(pane, IDC_CBO_FONT)).left());
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
        {
            MeetingHandlerWindow window(settings, layout);
            QVERIFY(window.captionsReady());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto* workspace = window.findChild<QWidget*>("workspace");
            QVERIFY(workspace && window.findChild<QWidget*>("workspaceSplitter") == nullptr);
            verifyCaptionFits(workspace);
            QVERIFY(!QIcon(":/icons/meeting-handler.ico").pixmap(16, 16).isNull());
            auto* captions = workspace->findChild<CaptionPane*>();
            QCOMPARE(captions->grab(QRect(0, 0, 8, 8)).toImage().pixelColor(1, 1),
                     QColor(32, 32, 32));
            auto* controller = workspace->findChild<MeetingControllerPane*>();
            QVERIFY(controller && !controller->isWindow() && !controller->styleSheet().isEmpty());
            auto* log = window.findChild<QPlainTextEdit*>("appLog");
            auto* logPanel = window.findChild<QWidget*>("appLogPanel");
            QVERIFY(log && logPanel && !logPanel->isVisible());
            QVERIFY(log->toPlainText().contains("Ready"));
            const HWND frame = reinterpret_cast<HWND>(window.winId());
            native = reinterpret_cast<HWND>(captions->winId());
            QVERIFY(native && ::GetAncestor(native, GA_ROOT) == frame);
            QVERIFY((::GetWindowLongPtrW(native, GWL_STYLE) & WS_CHILD) != 0);
            QVERIFY(window.windowTitle().contains("No target selected"));
            const HWND view = ::GetDlgItem(native, IDC_CAPTION_VIEW);
            const HWND listen = ::GetDlgItem(native, IDC_BTN_LISTEN);
            QVERIFY(view && listen);
            ::SendMessageW(listen, BM_CLICK, 0, 0);
            QCOMPARE(windowText(listen), "Resume caption listening");

            verifyCaptionFits(workspace);

            QCOMPARE(window.findChildren<QPushButton*>("pageNavButton").size(), 0);
            QVERIFY(window.findChild<QWidget*>("pageStack") == nullptr);
            ::SendMessageW(::GetDlgItem(native, IDC_BTN_LOG), BM_CLICK, 0, 0);
            QTRY_VERIFY(logPanel->isVisible());
            QVERIFY(::IsWindowVisible(view));
            ::SendMessageW(::GetDlgItem(native, IDC_BTN_LOG), BM_CLICK, 0, 0);
            QTRY_VERIFY(!logPanel->isVisible());

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
            QTRY_VERIFY(controller->isAncestorOf(QApplication::focusWidget()));
            QVERIFY(!::IsChild(native, ::GetFocus()));
            auto* update = new CaptionUpdate{0, {L"Combined caption test"}};
            ::SendMessageW(native, WM_APP_CAPTION_UPDATE, 0, reinterpret_cast<LPARAM>(update));
            ::SendMessageW(view, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(5, 15));
            ::SendMessageW(view, WM_LBUTTONUP, 0, MAKELPARAM(5, 15));
            ::SendMessageW(::GetDlgItem(native, IDC_BTN_SEND), BM_CLICK, 0, 0);
            QVERIFY(log->toPlainText().contains("Pick a web tab before sending."));
            ::PostMessageW(view, WM_KEYDOWN, VK_ESCAPE, 0);
            ::PostMessageW(native, WM_HOTKEY, HOTKEY_SEND, 0);
            QTRY_VERIFY(log->toPlainText().contains("Select caption text before sending."));

            dialogOwner = frame;
            QVERIFY(::SetTimer(nullptr, 0, 25, dismissCaptionSettings) != 0);
            ::SendMessageW(::GetDlgItem(native, IDC_BTN_SETTINGS), BM_CLICK, 0, 0);
            QVERIFY(foundSettings);
            QVERIFY(::IsWindowEnabled(frame));

            QTimer::singleShot(0, &window, &QWidget::close);
            QCOMPARE(QApplication::exec(), 0);
            QVERIFY(layout.value("splitter").toByteArray().isEmpty());
            QVERIFY(!layout.value("geometry").toByteArray().isEmpty());
        }
        QVERIFY(!::IsWindow(native));
        settings.theme = UiTheme::Light;
        MeetingHandlerWindow reopened(settings, layout);
        QVERIFY(reopened.captionsReady());
        auto* workspace = reopened.findChild<QWidget*>("workspace");
        QVERIFY(workspace && reopened.findChild<QWidget*>("workspaceSplitter") == nullptr);
        reopened.show();
        QVERIFY(QTest::qWaitForWindowExposed(&reopened));
        verifyCaptionFits(workspace);
        QCOMPARE(workspace->findChild<CaptionPane*>()->grab(QRect(0, 0, 8, 8)).toImage().pixelColor(1, 1),
                 QColor(Qt::white));
        reopened.showMaximized();
        QTRY_VERIFY(reopened.isMaximized());
        QCoreApplication::processEvents();
        verifyCaptionFits(workspace);
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
