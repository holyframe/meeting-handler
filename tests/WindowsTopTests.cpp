#include "ui/WindowsTop.h"
#include "ui/CaptionPane.h"
#include "resource.h"

#include <QApplication>
#include <QLabel>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <windowsx.h>

namespace {
HWND handle(QWidget& w) { return reinterpret_cast<HWND>(w.winId()); }

QWidget* toolWindow(const char* name) {
    for (auto* widget : QApplication::topLevelWidgets())
        if (widget->objectName() == name) return widget;
    return nullptr;
}

bool hasStyle(HWND hwnd, LONG_PTR mask) {
    return (::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & mask) != 0;
}

int alpha(HWND hwnd) {
    BYTE a = 0;
    return ::GetLayeredWindowAttributes(hwnd, nullptr, &a, nullptr) ? a : -1;
}

constexpr UINT allowPreviewActivation = WM_APP + 1;

class PreviewSource final : public QWidget {
public:
    QPoint lastClick{-1, -1};
    int releases = 0;
protected:
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override {
        const auto* msg = static_cast<MSG*>(message);
        if (msg->message == allowPreviewActivation) {
            *result = ::AllowSetForegroundWindow(static_cast<DWORD>(msg->wParam));
            return true;
        }
        if (msg->message == WM_LBUTTONUP) {
            ++releases;
            lastClick = QPoint(GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam));
        }
        return QWidget::nativeEvent(type, message, result);
    }
};

HWND fixtureWindow(DWORD pid) {
    struct Search { DWORD pid; HWND result; } search{pid, nullptr};
    ::EnumWindows([](HWND hwnd, LPARAM data) -> BOOL {
        auto& match = *reinterpret_cast<Search*>(data);
        DWORD owner = 0;
        ::GetWindowThreadProcessId(hwnd, &owner);
        if (owner != match.pid || !::FindWindowExW(hwnd, nullptr, L"EDIT", nullptr)) return TRUE;
        match.result = hwnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.result;
}
}

class WindowsTopTests final : public QObject {
    Q_OBJECT
private slots:
    void pinBorderAndMaximize() {
        QWidget source;
        source.resize(370, 230);
        source.show();
        const auto hwnd = handle(source);
        WindowsTop top;
        WindowsTopSettings settings;
        settings.suspendWhenMaximized = true;
        settings.borderRgb = 0x127faa;
        settings.borderWidth = 600;
        top.configure(settings);
        QVERIFY(!hasStyle(hwnd, WS_EX_TOPMOST));
        QCOMPARE(top.toggle(WindowAction::Pin, hwnd), QString());
        QVERIFY(hasStyle(hwnd, WS_EX_TOPMOST));
        auto* border = toolWindow("pinnedWindowBorder");
        QVERIFY(border && border->isVisible());
        QCOMPARE(border->palette().color(QPalette::Window), QColor::fromRgb(settings.borderRgb));
        const int thickness = ::MulDiv(6, ::GetDpiForWindow(handle(*border)), 96);
        HRGN region = ::CreateRectRgn(0, 0, 0, 0);
        QVERIFY(::GetWindowRgn(handle(*border), region) != ERROR);
        const bool ring = ::PtInRegion(region, thickness - 1, 40) && !::PtInRegion(region, thickness, 40);
        ::DeleteObject(region);
        QVERIFY(ring);
        source.showMaximized();
        QTRY_VERIFY(!hasStyle(hwnd, WS_EX_TOPMOST));
        QVERIFY(!toolWindow("pinnedWindowBorder"));
        source.showNormal();
        QTRY_VERIFY(hasStyle(hwnd, WS_EX_TOPMOST));
        settings.highlight = false;
        top.configure(settings);
        QVERIFY(!toolWindow("pinnedWindowBorder"));
        QCOMPARE(top.toggle(WindowAction::Pin, hwnd), QString());
        QVERIFY(!hasStyle(hwnd, WS_EX_TOPMOST));
    }

    void restoresLayeredAndTopmostState() {
        QWidget source, layered;
        layered.setWindowFlag(Qt::WindowStaysOnTopHint);
        source.show();
        layered.show();
        const auto a = handle(source), b = handle(layered);
        QTRY_VERIFY(hasStyle(b, WS_EX_TOPMOST));
        ::SetWindowLongPtrW(b, GWL_EXSTYLE, ::GetWindowLongPtrW(b, GWL_EXSTYLE) | WS_EX_LAYERED);
        constexpr COLORREF key = RGB(9, 13, 15);
        QVERIFY(::SetLayeredWindowAttributes(b, key, 181, LWA_ALPHA | LWA_COLORKEY));
        QVERIFY(!hasStyle(a, WS_EX_LAYERED));
        {
            WindowsTop top;
            WindowsTopSettings settings;
            settings.opacityPercent = 37;
            settings.highlight = false;
            top.configure(settings);
            for (const auto hwnd : {a, b}) {
                QCOMPARE(top.toggle(WindowAction::Pin, hwnd), QString());
                QCOMPARE(top.toggle(WindowAction::Opacity, hwnd), QString());
                QCOMPARE(alpha(hwnd), ::MulDiv(37, 255, 100));
            }
            settings.opacityPercent = 55;
            top.configure(settings);
            QCOMPARE(alpha(b), ::MulDiv(55, 255, 100));
            QCOMPARE(top.toggle(WindowAction::Opacity, b), QString());
            QCOMPARE(alpha(b), 181);
            QVERIFY(hasStyle(b, WS_EX_TOPMOST));
        }
        QVERIFY(!hasStyle(a, WS_EX_LAYERED) && !hasStyle(a, WS_EX_TOPMOST));
        QVERIFY(hasStyle(b, WS_EX_TOPMOST));
        COLORREF restored = 0;
        DWORD flags = 0;
        QVERIFY(::GetLayeredWindowAttributes(b, &restored, nullptr, &flags));
        QCOMPARE(restored, key);
        QCOMPARE(flags, DWORD(LWA_ALPHA | LWA_COLORKEY));
    }

    void hoverTracksTheWindowUnderThePointer() {
        QWidget source, child(&source), other;
        source.resize(240, 160);
        source.show();
        const auto hwnd = handle(source);
        // Drive only the desktop lookup. Child-to-root matching and opacity use native windows.
        HWND hovered = handle(child);
        WindowsTop::DesktopWindows desktop;
        desktop.underPointer = [&] { return hovered; };
        WindowsTop top(desktop);
        WindowsTopSettings settings;
        settings.opaqueOnHover = true;
        top.configure(settings);
        QCOMPARE(top.toggle(WindowAction::Opacity, hwnd), QString());
        QTRY_COMPARE(alpha(hwnd), 255);
        hovered = handle(other);
        QTRY_COMPARE(alpha(hwnd), ::MulDiv(settings.opacityPercent, 255, 100));
        hovered = hwnd;
        QTRY_COMPARE(alpha(hwnd), 255);
    }

    void livePreviewCropInputAndSourceClosure() {
        WindowsTop top;
        WindowsTopSettings settings;
        settings.interactivePreview = false;
        top.configure(settings);
        QPointer<QWidget> preview;
        {
            PreviewSource source;
            source.resize(480, 300);
            source.show();
            const auto hwnd = handle(source);
            QCOMPARE(top.toggle(WindowAction::PictureInPicture, hwnd), QString());
            preview = toolWindow("windowPreview");
            QVERIFY(preview && preview->isVisible());
            auto* canvas = preview->findChild<QLabel*>("previewCanvas");
            auto* full = preview->findChild<QPushButton*>("previewShowFull");
            auto* crop = preview->findChild<QPushButton*>("previewCrop");
            QVERIFY(canvas && full && crop);
            const QPoint center = canvas->rect().center();
            QTest::mouseClick(canvas, Qt::LeftButton, {}, center);
            QCoreApplication::processEvents();
            QCOMPARE(source.releases, 0);

            settings.interactivePreview = true;
            top.configure(settings);
            QTest::mouseClick(canvas, Qt::LeftButton, {}, center);
            QTRY_COMPARE(source.releases, 1);
            const QPoint original = source.lastClick;
            QCOMPARE(top.toggle(WindowAction::Crop, handle(*preview)), QString());
            QVERIFY(crop->isChecked());
            QTest::mousePress(canvas, Qt::LeftButton, {}, center);
            QTest::mouseRelease(canvas, Qt::LeftButton, {}, center + QPoint(70, 40));
            QVERIFY(full->isEnabled() && !crop->isChecked());
            QCOMPARE(source.releases, 1);
            QTest::mouseClick(canvas, Qt::LeftButton, {}, center);
            QTRY_COMPARE(source.releases, 2);
            QVERIFY(source.lastClick.x() > original.x() && source.lastClick.y() > original.y());
            full->click();
            QVERIFY(!full->isEnabled());
            QTest::mouseClick(canvas, Qt::LeftButton, {}, center);
            QTRY_COMPARE(source.releases, 3);
            QCOMPARE(source.lastClick, original);
            source.showMinimized();
            QTRY_VERIFY(canvas->text().contains("paused"));
            source.showNormal();
            QTRY_VERIFY(canvas->text().isEmpty());
            QCOMPARE(top.toggle(WindowAction::PictureInPicture, handle(*preview)), QString());
            QVERIFY(!preview);
            QCOMPARE(top.toggle(WindowAction::Crop, hwnd), QString());
            preview = toolWindow("windowPreview");
            QVERIFY(preview && preview->findChild<QPushButton*>("previewCrop")->isChecked());
        }
        QTRY_VERIFY(!preview);
    }

    void shortcutsAndPreviewControlASeparateApplication() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        Settings settings;
        settings.captionSource = CaptionSourcePreference::ChromeLiveCaption;
        settings.transcriptPath = temp.filePath("test-captions.txt").toStdWString();
        settings.hotkeyVirtualKey = 0;
        settings.windowsTop.highlight = false;
        constexpr unsigned mods = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
        for (size_t i = 0; i < windowActionCount; ++i)
            settings.windowsTop.shortcuts[i] = {mods, VK_F20 + unsigned(i)};
        QWidget owner, sidebar;
        HWND selected = nullptr;
        bool nativeForeground = false;
        WindowsTop::DesktopWindows desktop;
        desktop.foreground = [&] { return nativeForeground ? ::GetForegroundWindow() : selected; };
        CaptionPane pane(&sidebar, &owner, {}, desktop);
        QVERIFY(pane.initialize(settings));

        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--preview-fixture"});
        QVERIFY(child.waitForStarted());
        const auto pid = DWORD(child.processId());
        const auto close = qScopeGuard([&] {
            if (auto hwnd = fixtureWindow(pid)) ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
            child.waitForFinished(3000);
        });
        HWND source = nullptr;
        QTRY_VERIFY((source = fixtureWindow(pid)) != nullptr);
        const HWND edit = ::FindWindowExW(source, nullptr, L"EDIT", nullptr);
        QVERIFY(edit);
        selected = source;
        nativeForeground = ::SetForegroundWindow(source) &&
            QTest::qWaitFor([&] { return ::GetForegroundWindow() == source; }, 300);
        const auto shortcut = [&](WindowAction action) {
            const int id = int(action);
            return ::PostMessageW(handle(owner), WM_HOTKEY, HOTKEY_WINDOW_FIRST + id, MAKELPARAM(mods, VK_F20 + id));
        };
        QVERIFY(shortcut(WindowAction::Pin));
        QTRY_VERIFY(hasStyle(source, WS_EX_TOPMOST));
        QVERIFY(shortcut(WindowAction::Opacity));
        QTRY_COMPARE(alpha(source), ::MulDiv(settings.windowsTop.opacityPercent, 255, 100));
        QVERIFY(shortcut(WindowAction::Pin));
        QTRY_VERIFY(!hasStyle(source, WS_EX_TOPMOST));
        QVERIFY(shortcut(WindowAction::Opacity));
        QTRY_VERIFY(!hasStyle(source, WS_EX_LAYERED));
        QVERIFY(shortcut(WindowAction::PictureInPicture));
        QTRY_VERIFY(toolWindow("windowPreview"));
        auto* preview = toolWindow("windowPreview");
        // A synthetic click cannot grant foreground permission like a physical click.
        if (nativeForeground)
            QVERIFY(::SendMessageW(source, allowPreviewActivation, ::GetCurrentProcessId(), 0));
        preview->activateWindow();
        if (nativeForeground) QTRY_COMPARE(::GetForegroundWindow(), handle(*preview));
        auto* canvas = preview->findChild<QLabel*>("previewCanvas");
        QVERIFY(canvas);
        QTest::mouseClick(canvas, Qt::LeftButton, {}, canvas->rect().center());
        // A center click lands after the fixture text, even on a locked desktop.
        QTRY_COMPARE(LOWORD(::SendMessageW(edit, EM_GETSEL, 0, 0)), WORD(12));
        if (nativeForeground) {
            QTRY_COMPARE(::GetForegroundWindow(), source);
            GUITHREADINFO info{sizeof(GUITHREADINFO)};
            const DWORD thread = ::GetWindowThreadProcessId(source, nullptr);
            QTRY_VERIFY(::GetGUIThreadInfo(thread, &info) && info.hwndFocus == edit);
        }
        QVERIFY(shortcut(WindowAction::Crop));
        QTRY_VERIFY(preview->findChild<QPushButton*>("previewCrop")->isChecked());
        selected = handle(*preview);
        QVERIFY(shortcut(WindowAction::PictureInPicture));
        QTRY_VERIFY(!toolWindow("windowPreview"));
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (app.arguments().contains("--preview-fixture")) {
        PreviewSource source;
        source.setWindowTitle("Meeting Handler preview test");
        source.resize(360, 240);
        source.show();
        ::ShowWindow(handle(source), SW_SHOWNOACTIVATE);
        RECT client{};
        ::GetClientRect(handle(source), &client);
        if (!::CreateWindowExW(0, L"EDIT", L"Preview test", WS_CHILD | WS_VISIBLE | ES_MULTILINE,
                               0, 0, client.right, client.bottom, handle(source), nullptr, nullptr, nullptr)) return 1;
        return app.exec();
    }
    WindowsTopTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "WindowsTopTests.moc"
