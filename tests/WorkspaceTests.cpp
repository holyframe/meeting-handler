#include "ui/MeetingHandlerWindow.h"
#include "ui/MeetingControllerPane.h"
#include "ui/AudioLevelControl.h"
#include "ui/CaptionPane.h"
#include "ui/CaptionView.h"
#include "ui/PingStatusPanel.h"
#include "ui/SettingsDialog.h"
#include "ui/WindowPickOutline.h"
#include "captions/CaptureEngine.h"
#include "resource.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QSlider>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

namespace {
QRect boundsIn(QWidget* widget, QWidget* parent) {
    return {widget->mapTo(parent, QPoint()), widget->size()};
}

QRect nativeBounds(HWND window) {
    RECT r{};
    if (!::GetWindowRect(window, &r)) return {};
    return {r.left, r.top, r.right - r.left, r.bottom - r.top};
}

void verifyControlsFit(QWidget* bar, std::initializer_list<const char*> names) {
    QList<QRect> seen;
    for (const auto* name : names) {
        auto* control = bar->findChild<QWidget*>(name);
        QVERIFY2(control && control->isVisible(), name);
        const QRect rect = boundsIn(control, bar);
        QVERIFY2(!rect.isEmpty() && bar->rect().contains(rect), name);
        for (const auto& other : seen) QVERIFY2(!rect.intersects(other), name);
        seen.append(rect);
    }
}

void showWithPingFixture(MeetingHandlerWindow& window) {
    auto* panel = window.findChild<PingStatusPanel*>();
    auto* ping = panel ? panel->findChild<QProcess*>() : nullptr;
    QVERIFY(ping && ping->state() == QProcess::NotRunning);
    // Exercise startup without depending on DNS, ICMP permissions or an external host.
    ping->setProgram(QCoreApplication::applicationFilePath());
    ping->setArguments({"--ping-stream-fixture"});
    window.show();
}

void verifyColumnsFit(QWidget* workspace) {
    auto* columns = workspace->findChild<QSplitter*>("workspaceColumns");
    auto* sidebar = workspace->findChild<QWidget*>("captionSidebar");
    auto* captions = workspace->findChild<CaptionPane*>();
    auto* controller = workspace->findChild<MeetingControllerPane*>();
    QVERIFY(columns && sidebar && captions && controller);
    QVERIFY(!workspace->findChild<QWidget*>("windowDragStrip"));
    QCOMPARE(boundsIn(columns, workspace).top(), 0);
    auto* picker = sidebar->findChild<QToolButton*>("pickCaptionTarget");
    auto* more = sidebar->findChild<QToolButton*>("showPingStatus");
    auto* ping = sidebar->findChild<PingStatusPanel*>();
    auto* targetList = sidebar->findChild<QWidget*>("targetScroll");
    auto* meetingHeader = controller->findChild<QWidget*>("collapseBar");
    QVERIFY(picker && more && ping && targetList && meetingHeader);
    QVERIFY(!sidebar->findChild<QLabel*>("captionTargetHeading"));
    QVERIFY(picker->isVisible());
    const auto pickerRect = boundsIn(picker, workspace);
    const auto headerRect = boundsIn(meetingHeader, workspace);
    QCOMPARE(headerRect.top(), 0);
    QCOMPARE(pickerRect.center().y(), headerRect.center().y());
    QVERIFY(qAbs(pickerRect.center().x() - boundsIn(sidebar, workspace).center().x()) <= 1);
    QCOMPARE(boundsIn(targetList, workspace).top(), headerRect.bottom() + 1);
    QVERIFY(more->isVisible());
    QCOMPARE(targetList->geometry().bottom() + 1, more->y());
    QCOMPARE(more->height(), columns->handleWidth());
    QCOMPARE(more->width(), sidebar->width());
    QVERIFY(more->isChecked() && ping->isVisible());
    QCOMPARE(more->geometry().bottom() + 1, ping->y());
    QCOMPARE(ping->geometry().bottom(), sidebar->rect().bottom());
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
    auto* formatting = captions->findChild<QWidget*>("captionFormattingBar");
    auto* toolbar = captions->findChild<QWidget*>("captionToolbar");
    QVERIFY(view && formatting && toolbar);
    QCOMPARE(boundsIn(formatting, captions).topLeft(), QPoint());
    QCOMPARE(formatting->height(), meetingHeader->height());
    const auto background = [](QWidget* pane) { return pane->grab(QRect(0, 0, 2, 2)).toImage().pixelColor(0, 0); };
    const auto captionColor = background(formatting);
    QCOMPARE(captionColor, captions->palette().color(QPalette::AlternateBase));
    QCOMPARE(background(sidebar), captionColor);
    QCOMPARE(background(meetingHeader), captionColor);
    QCOMPARE(formatting->width(), captions->width());
    QCOMPARE(boundsIn(view, captions).topLeft(), QPoint(0, formatting->height()));
    QCOMPARE(boundsIn(view, captions).right(), captions->rect().right());
    QCOMPARE(boundsIn(toolbar, captions).bottom(), captions->rect().bottom());
    verifyControlsFit(formatting, {"captionFont", "captionSize", "captionSpacing"});
    verifyControlsFit(toolbar, {"sendCaption", "pressEnter", "toggleListening", "saveCaptions", "clearCaptions",
                                "captionPreferences", "toggleActivityLog", "microphoneStatus", "microphoneVolume",
                                "microphonePeak", "microphonePercent", "speakerMute", "speakerVolume",
                                "speakerPeak", "speakerPercent"});
}
}

class WorkspaceTests : public QObject {
    Q_OBJECT
private slots:
    void pickerOutlineFollowsNativeFrames() {
        QList<QRect> displays;
        QVERIFY(::EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT r, LPARAM data) -> BOOL {
            auto& list = *reinterpret_cast<QList<QRect>*>(data);
            const QRect screen(r->left, r->top, r->right - r->left, r->bottom - r->top);
            if (screen.contains(QPoint(0, 0))) list.prepend(screen);
            else list.append(screen);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&displays)));
        QVERIFY(!displays.isEmpty());

        QWidget owner;
        owner.setAttribute(Qt::WA_ShowWithoutActivating);
        owner.setGeometry(20, 20, 80, 60);
        owner.show();
        QWidget target(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        target.setAttribute(Qt::WA_ShowWithoutActivating);
        target.show();
        WindowPickOutline outline(&owner);
        const auto targetWindow = reinterpret_cast<HWND>(target.winId());
        const auto checkFrame = [&](QRect frame) {
            QVERIFY(::SetWindowPos(targetWindow, HWND_TOP, frame.x(), frame.y(), frame.width(), frame.height(),
                                   SWP_NOACTIVATE));
            QCoreApplication::processEvents();
            QCOMPARE(nativeBounds(targetWindow), frame);
            outline.showAround(targetWindow);
            QCoreApplication::processEvents();
            QVERIFY(outline.isVisible());
            const auto overlay = reinterpret_cast<HWND>(outline.winId());
            QTRY_COMPARE(nativeBounds(overlay), frame);
            QCOMPARE(::MonitorFromWindow(overlay, MONITOR_DEFAULTTONEAREST),
                     ::MonitorFromWindow(targetWindow, MONITOR_DEFAULTTONEAREST));
            QVERIFY(::GetForegroundWindow() != overlay);
            QVERIFY(::WindowFromPoint({frame.center().x(), frame.center().y()}) != overlay);
            QVERIFY(::WindowFromPoint({frame.left() + 1, frame.top() + 1}) != overlay);
            HRGN nativeRegion = ::CreateRectRgn(0, 0, 0, 0);
            QVERIFY(nativeRegion);
            const int kind = ::GetWindowRgn(overlay, nativeRegion);
            const QRegion ring = QRegion::fromHRGN(nativeRegion);
            ::DeleteObject(nativeRegion);
            QCOMPARE(kind, int(COMPLEXREGION));
            QCOMPARE(ring.boundingRect(), QRect(QPoint(), frame.size()));
            QVERIFY(!ring.contains(ring.boundingRect().center()));
            QVERIFY(ring.contains(QPoint(1, 1)) && ring.contains(QPoint(frame.width() - 2, frame.height() - 2)));
            const QImage pixels = outline.grab().toImage();
            QVERIFY(!pixels.isNull());
            QCOMPARE(pixels.pixelColor(1, 1), QColor(Qt::red));
            QCOMPARE(pixels.pixelColor(pixels.width() - 2, pixels.height() - 2), QColor(Qt::red));
        };

        // Start on a secondary display, then reuse the visible overlay across monitors.
        for (auto it = displays.crbegin(); it != displays.crend(); ++it) {
            qInfo() << "Checking picker on native display" << *it;
            checkFrame(QRect(it->topLeft() + QPoint(80, 60), QSize(320, 180)));
        }
        outline.hide();
        checkFrame(displays.last()); // All four edges of a full-screen target must be visible.
        checkFrame(QRect(displays.first().topLeft() - QPoint(35, 20), QSize(280, 180)));
        outline.showAround(nullptr);
        QVERIFY(!outline.isVisible());
    }

    void sidebarPingStartsOpenStreamsAndStops() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        Settings settings;
        settings.captionSource = CaptionSourcePreference::ChromeLiveCaption;
        settings.transcriptPath = temp.filePath("captions.txt").toStdWString();
        settings.hotkeyVirtualKey = 0;
        QSettings layout(temp.filePath("layout.ini"), QSettings::IniFormat);
        MeetingHandlerWindow window(settings, layout);
        QVERIFY(window.captionsReady());
        window.resize(1100, 520);

        auto* sidebar = window.findChild<QWidget*>("captionSidebar");
        auto* columns = window.findChild<QSplitter*>("workspaceColumns");
        QVERIFY(sidebar && columns);
        auto* panel = sidebar->findChild<PingStatusPanel*>("pingStatusPanel");
        auto* toggle = sidebar->findChild<QToolButton*>("showPingStatus");
        auto* targets = sidebar->findChild<QWidget*>("targetScroll");
        QVERIFY(panel && toggle && targets);
        QCOMPARE(panel->parentWidget(), sidebar);
        QCOMPARE(panel->window(), &window);
        QVERIFY(!panel->isWindow() && toggle->isCheckable());

        auto* ping = panel->findChild<QProcess*>();
        auto* output = panel->findChild<QPlainTextEdit*>("pingOutput");
        auto* retry = panel->findChild<QTimer*>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(ping && output && retry);
        const QString program = ping->program();
        const QFileInfo executable(program);
        QVERIFY(executable.isAbsolute() && executable.isFile());
        QCOMPARE(executable.fileName().toLower(), "ping.exe");
        QCOMPARE(ping->arguments(), QStringList({"google.com", "-t"}));
        QCOMPARE(ping->state(), QProcess::NotRunning);
        QVERIFY(output->isReadOnly() && output->maximumBlockCount() > 0);
        const QString unavailable = QStringLiteral("bytes=— time=— TTL=—");
        QCOMPARE(output->placeholderText(), unavailable);
        QVERIFY(panel->findChildren<QLabel*>().isEmpty());
        QCOMPARE(output->lineWrapMode(), QPlainTextEdit::WidgetWidth);
        QCOMPARE(output->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);

        QSignalSpy starts(ping, &QProcess::started);
        showWithPingFixture(window);
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_COMPARE(starts.size(), 1);
        QVERIFY(panel->isVisible() && toggle->isChecked());
        QCOMPARE(toggle->accessibleName(), "Hide ping status for google.com");
        const auto widths = columns->sizes();
        const QSize stripSize = toggle->size();
        const int expandedTargetHeight = targets->height();
        QCOMPARE(stripSize, QSize(sidebar->width(), columns->handleWidth()));
        QVERIFY(expandedTargetHeight > panel->height());
        QCOMPARE(targets->geometry().bottom() + 1, toggle->y());
        QCOMPARE(toggle->geometry().bottom() + 1, panel->y());
        QCOMPARE(panel->geometry().bottom(), sidebar->rect().bottom());
        QVERIFY(sidebar->rect().contains(panel->geometry()));
        QVERIFY(panel->rect().contains(output->geometry()));
        const auto expected = QStringList({
            unavailable,
            "bytes=32 time=17ms TTL=117",
            "bytes=32 time<1ms TTL=128",
            "bytes=64 time=23ms TTL=59",
            QStringLiteral("bytes=— time=9ms TTL=—"),
            unavailable
        }).join(u'\n');
        QTRY_COMPARE(output->toPlainText(), expected);
        QCOMPARE(starts.size(), 1);
        QTest::keyClick(toggle, Qt::Key_Space);
        QVERIFY(panel->isHidden() && !toggle->isChecked());
        QCOMPARE(toggle->accessibleName(), "Show ping status for google.com");
        QTRY_VERIFY(targets->height() > expandedTargetHeight);
        QCOMPARE(toggle->size(), stripSize);
        QCOMPARE(toggle->geometry().bottom(), sidebar->rect().bottom());
        QCOMPARE(columns->sizes(), widths);
        QCOMPARE(ping->state(), QProcess::NotRunning);
        QVERIFY(!retry->isActive());

        const QImage grip = toggle->grab().toImage();
        const int middleRow = grip.height() / 2;
        const QRgb background = grip.pixel(grip.width() / 4, middleRow);
        int dotCount = 0;
        bool insideDot = false;
        for (int x = grip.width() / 4; x < 3 * grip.width() / 4; ++x) {
            const bool ink = grip.pixel(x, middleRow) != background;
            if (ink && !insideDot) ++dotCount;
            insideDot = ink;
        }
        QCOMPARE(dotCount, 3);

        columns->setSizes({240, widths.at(1), widths.at(2) - (240 - widths.first())});
        const auto widerWidths = columns->sizes();
        ping->setProgram(temp.filePath("missing-ping.exe"));
        toggle->click();
        QTRY_VERIFY(retry->isActive());
        QCOMPARE(output->toPlainText(), unavailable);
        QVERIFY(output->toolTip().contains("Could not start ping"));
        QCOMPARE(columns->sizes(), widerWidths);
        QCOMPARE(panel->width(), sidebar->width());
        QCOMPARE(toggle->width(), sidebar->width());
        QCOMPARE(toggle->geometry().bottom() + 1, panel->y());
        QTest::mouseClick(toggle, Qt::LeftButton, {}, QPoint(toggle->width() - 2, toggle->height() / 2));
        QVERIFY(!retry->isActive());
        toggle->click();
        QTRY_VERIFY(retry->isActive());
        ping->setProgram(QCoreApplication::applicationFilePath());
        retry->setInterval(200);
        QTRY_COMPARE(starts.size(), 2);
        ping->kill();
        QTRY_COMPARE(starts.size(), 3);
        QVERIFY(window.close());
        QCOMPARE(ping->state(), QProcess::NotRunning);
        QVERIFY(!retry->isActive());
    }

    void microphoneIconOnlyDisplaysState() {
        AudioLevelControl control(AudioEndpoint::Microphone, nullptr);
        auto* icon = control.findChild<QLabel*>("microphoneStatus");
        auto* slider = control.findChild<QSlider*>("microphoneVolume");
        QVERIFY(icon && slider && !control.findChild<QToolButton*>());
        QSignalSpy muteWrites(&control, &AudioLevelControl::muteRequested);
        QSignalSpy volumeWrites(&control, &AudioLevelControl::volumeRequested);
        control.setLevels(AudioEndpoint::Microphone, {"mic", 55, 10, false});
        control.show();
        const auto onGlyph = icon->text();
        QCOMPARE(icon->accessibleName(), "Windows microphone is on");
        QCOMPARE(icon->focusPolicy(), Qt::NoFocus);
        QTest::mouseClick(icon, Qt::LeftButton);
        QTest::keyClick(icon, Qt::Key_Space);
        control.setLevels(AudioEndpoint::Microphone, {"mic", 55, 0, true});
        QCOMPARE(icon->accessibleName(), "Windows microphone is muted");
        QVERIFY(icon->text() != onGlyph);
        QTest::mouseClick(icon, Qt::LeftButton);
        QVERIFY(muteWrites.isEmpty() && volumeWrites.isEmpty());
        QTest::keyClick(slider, Qt::Key_Right);
        QCOMPARE(volumeWrites.size(), 1);
        QVERIFY(muteWrites.isEmpty());
    }

    void windowsSpeakerMuteIcon() {
        const auto endpoint = AudioEndpoint::Speakers;
        AudioLevelControl control(endpoint, nullptr);
        auto* button = control.findChild<QToolButton*>("speakerMute");
        auto* slider = control.findChild<QSlider*>();
        QVERIFY(button && slider);
        QSignalSpy requests(&control, &AudioLevelControl::muteRequested);
        QSignalSpy volumeWrites(&control, &AudioLevelControl::volumeRequested);
        QVERIFY(button->isCheckable() && !button->isEnabled());
        control.setLevels(AudioEndpoint::Microphone, {"other", 90, 0, true});
        QVERIFY(!button->isEnabled());

        control.setLevels(endpoint, {"device A", 55, 10, false});
        control.show();
        QVERIFY(button->isEnabled() && !button->isChecked());
        const QString onGlyph = button->text();
        QCOMPARE(onGlyph.size(), 1);
        QVERIFY(button->fontMetrics().inFont(onGlyph.front()));
        QCOMPARE(button->accessibleName(), "Mute Windows sound");
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(requests.size(), 1);
        const auto mute = requests.takeFirst();
        QCOMPARE(mute.at(0).value<AudioEndpoint>(), endpoint);
        QCOMPARE(mute.at(1).toString(), "device A");
        QCOMPARE(mute.at(2).toBool(), true);
        QVERIFY(!button->isChecked());

        control.setLevels(endpoint, {"device A", 55, 0, true});
        QVERIFY(button->isChecked());
        QVERIFY(button->text() != onGlyph && button->fontMetrics().inFont(button->text().front()));
        QCOMPARE(button->accessibleName(), "Unmute Windows sound");
        QCOMPARE(slider->value(), 55);
        QVERIFY(requests.isEmpty());
        QTest::keyClick(button, Qt::Key_Space);
        QCOMPARE(requests.size(), 1);
        QCOMPARE(requests.takeFirst().at(2).toBool(), false);
        control.setLevels(endpoint, {"device A", 55, 4, false});
        QCOMPARE(button->text(), onGlyph);

        QTest::mousePress(button, Qt::LeftButton);
        QVERIFY(button->isDown());
        control.setLevels(endpoint, {"device B", 32, 4, false});
        QTest::mouseRelease(button, Qt::LeftButton);
        QVERIFY(requests.isEmpty());
        control.setLevels(endpoint, {"device B", 32, 0, std::nullopt});
        QVERIFY(!button->isEnabled() && slider->isEnabled());
        button->click();
        QVERIFY(requests.isEmpty() && volumeWrites.isEmpty());
    }

    void audioControlsFollowWindowsWithoutWritingOnRefresh() {
        QWidget owner;
        AudioLevelControl mic(AudioEndpoint::Microphone, &owner);
        AudioLevelControl speakers(AudioEndpoint::Speakers, &owner);
        auto* input = mic.findChild<QSlider*>("microphoneVolume");
        auto* output = speakers.findChild<QSlider*>("speakerVolume");
        auto* meter = mic.findChild<QProgressBar*>("microphonePeak");
        auto* percent = mic.findChild<QLabel*>("microphonePercent");
        QVERIFY(input && output && meter && percent);
        QSignalSpy inputWrites(&mic, &AudioLevelControl::volumeRequested);
        QSignalSpy outputWrites(&speakers, &AudioLevelControl::volumeRequested);
        QVERIFY(!input->isEnabled() && !output->isEnabled());
        QCOMPARE(percent->text(), QStringLiteral("—"));
        const auto update = [&](AudioEndpoint kind, const AudioLevels& levels) {
            mic.setLevels(kind, levels);
            speakers.setLevels(kind, levels);
        };
        update(AudioEndpoint::Microphone, {"mic A", 63, 24, false});
        update(AudioEndpoint::Speakers, {"output A", 31, 0, true});
        QVERIFY(input->isEnabled() && output->isEnabled());
        QCOMPARE(input->value(), 63);
        QCOMPARE(output->value(), 31);
        QCOMPARE(meter->value(), 24);
        QVERIFY(inputWrites.isEmpty() && outputWrites.isEmpty());

        QTest::keyClick(input, Qt::Key_Right);
        QCOMPARE(inputWrites.count(), 1);
        const auto write = inputWrites.takeFirst();
        QCOMPARE(write.at(0).value<AudioEndpoint>(), AudioEndpoint::Microphone);
        QCOMPARE(write.at(1).toString(), "mic A");
        QCOMPARE(write.at(2).toInt(), 64);
        update(AudioEndpoint::Microphone, {"mic A", 64, 79, false});
        QCOMPARE(percent->text(), "64%");
        QCOMPARE(meter->value(), 79);
        QVERIFY(inputWrites.isEmpty() && outputWrites.isEmpty());

        output->setSliderDown(true);
        output->setSliderPosition(37);
        output->setSliderDown(false);
        QCOMPARE(outputWrites.count(), 1);
        const auto outputWrite = outputWrites.takeFirst();
        QCOMPARE(outputWrite.at(0).value<AudioEndpoint>(), AudioEndpoint::Speakers);
        QCOMPARE(outputWrite.at(1).toString(), "output A");
        QCOMPARE(outputWrite.at(2).toInt(), 37);
        input->setSliderDown(true);
        input->setSliderPosition(89);
        update(AudioEndpoint::Microphone, {"mic B", 18, std::nullopt, false});
        input->setSliderDown(false);
        QCOMPARE(input->value(), 18);
        QVERIFY(!meter->isEnabled());
        QVERIFY(inputWrites.isEmpty());
        update(AudioEndpoint::Microphone, {});
        QVERIFY(!input->isEnabled() && output->isEnabled());
        QCOMPARE(percent->text(), QStringLiteral("—"));
        QVERIFY(inputWrites.isEmpty() && outputWrites.isEmpty());
    }

    void speakerVolumeArrowsRespectFocusAndSettings() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        Settings settings;
        settings.captionSource = CaptionSourcePreference::ChromeLiveCaption;
        settings.transcriptPath = temp.filePath("captions.txt").toStdWString();
        settings.hotkeyVirtualKey = 0;
        QSettings layout(temp.filePath("layout.ini"), QSettings::IniFormat);
        QList<bool> requests;
        MeetingHandlerWindow window(settings, layout);
        auto* captions = window.findChild<CaptionPane*>();
        auto* picker = window.findChild<QToolButton*>("pickCaptionTarget");
        auto* meetings = window.findChild<QPushButton*>("collapseMeetingsButton");
        auto* view = window.findChild<CaptionView*>();
        auto* spacing = window.findChild<QComboBox*>("captionSpacing");
        auto* mic = window.findChild<QSlider*>("microphoneVolume");
        auto* preferences = window.findChild<QToolButton*>("captionPreferences");
        QVERIFY(window.captionsReady() && captions && picker && meetings && view && spacing && mic && preferences);
        // Record the command without changing the machine's actual speaker volume.
        QVERIFY(disconnect(captions, &CaptionPane::speakerVolumeStepRequested, nullptr, nullptr));
        connect(captions, &CaptionPane::speakerVolumeStepRequested, &window,
                [&](bool increase) { requests.append(increase); });
        showWithPingFixture(window);
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        for (auto* widget : QList<QWidget*>{picker, meetings, view}) {
            widget->setFocus();
            QTest::keyClick(widget, Qt::Key_Up);
            QTest::keyClick(widget, Qt::Key_Down);
            QCOMPARE(requests, QList<bool>({true, false}));
            requests.clear();
        }
        QTest::keyClick(view, Qt::Key_Up, Qt::ShiftModifier);
        QTest::keyClick(view, Qt::Key_Down, Qt::ControlModifier);
        spacing->setFocus();
        const int previousSpacing = spacing->currentIndex();
        QTest::keyClick(spacing, Qt::Key_Down);
        QCOMPARE(spacing->currentIndex(), previousSpacing + 1);
        {
            const QSignalBlocker blockMicWrites(mic);
            mic->setEnabled(true);
            mic->setValue(40);
            mic->setFocus();
            QTest::keyClick(mic, Qt::Key_Up);
            QCOMPARE(mic->value(), 40 + mic->singleStep());
        }
        QMenu menu(&window);
        auto* first = menu.addAction("First");
        auto* second = menu.addAction("Second");
        view->setFocus();
        menu.popup(window.mapToGlobal(QPoint(20, 20)));
        menu.setActiveAction(first);
        QTest::keyClick(&menu, Qt::Key_Down);
        QCOMPARE(menu.activeAction(), second);
        menu.close();
        QVERIFY(requests.isEmpty());

        const auto setOption = [&](bool enabled) {
            bool previousValueFound = false;
            QTimer::singleShot(0, &window, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) return;
                if (auto* box = dialog->findChild<QCheckBox*>("speakerVolumeArrowKeys")) {
                    previousValueFound = box->isChecked() != enabled;
                    box->setFocus();
                    QTest::keyClick(box, Qt::Key_Down);
                    box->setChecked(enabled);
                }
                dialog->accept();
            });
            preferences->click();
            QVERIFY(previousValueFound && requests.isEmpty());
            view->setFocus();
        };
        setOption(false);
        QTest::keyClick(view, Qt::Key_Up);
        QTest::keyClick(view, Qt::Key_Down);
        QVERIFY(requests.isEmpty());
        setOption(true);
        QTest::keyClick(view, Qt::Key_Up);
        QTest::keyClick(view, Qt::Key_Up);
        QCOMPARE(requests, QList<bool>({true, true}));

        QWidget otherWindow;
        otherWindow.show();
        otherWindow.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&otherWindow));
        QTest::keyClick(&otherWindow, Qt::Key_Down);
        QCOMPARE(requests.size(), 2);
    }

    void listeningStartsWindowsCaptionsOnce() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        Settings settings;
        settings.captionSource = CaptionSourcePreference::WindowsLiveCaptions;
        settings.transcriptPath = temp.filePath("captions.txt").toStdWString();
        settings.hotkeyVirtualKey = 0;
        bool windowOpen = false;
        int launches = 0;
        QWidget owner, sidebar(&owner);
        CaptionPane pane(&sidebar, &owner, {
            [&] { return windowOpen; }, [&] { ++launches; return true; }
        });
        QVERIFY(pane.initialize(settings));
        auto* listen = pane.findChild<QToolButton*>("toggleListening");
        auto* view = pane.findChild<CaptionView*>();
        QVERIFY(listen && view);
        QCOMPARE(launches, 0);
        QCOMPARE(listen->accessibleName(), "Start Windows Live Captions and listen");
        listen->click();
        QCOMPARE(launches, 1);
        QVERIFY(!listen->isEnabled());
        listen->click();
        QTest::qWait(350);
        QCOMPARE(launches, 1);
        QVERIFY(!listen->isEnabled());

        windowOpen = true;
        QTRY_COMPARE(listen->accessibleName(), "Stop caption listening");
        QVERIFY(listen->isEnabled());
        listen->click();
        QCOMPARE(listen->accessibleName(), "Resume caption listening");
        QCoreApplication::postEvent(&pane, new CaptionUpdate(0, {L"Existing caption"}));
        QTRY_VERIFY(!view->Empty());
        listen->click();
        QCOMPARE(listen->accessibleName(), "Stop caption listening");
        QCOMPARE(QString::fromStdWString(view->FullText()), QStringLiteral("Existing caption\r\n"));
        QCOMPARE(launches, 1);
    }

    void failedWindowsStartCanRetryAndSwitchToChrome() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        Settings settings;
        settings.captionSource = CaptionSourcePreference::WindowsLiveCaptions;
        settings.transcriptPath = temp.filePath("captions.txt").toStdWString();
        settings.hotkeyVirtualKey = 0;
        int launches = 0;
        int windowChecks = 0;
        bool windowOpen = false;
        QWidget owner, sidebar(&owner);
        CaptionPane pane(&sidebar, &owner, {
            [&] { ++windowChecks; return windowOpen; }, [&] { return ++launches > 1; }
        });
        QVERIFY(pane.initialize(settings));
        auto* listen = pane.findChild<QToolButton*>("toggleListening");
        auto* preferences = pane.findChild<QToolButton*>("captionPreferences");
        QVERIFY(listen && preferences);
        listen->click();
        auto* warning = pane.findChild<QMessageBox*>();
        QVERIFY(warning && warning->isVisible());
        QVERIFY(listen->isEnabled());
        QCOMPARE(listen->accessibleName(), "Start Windows Live Captions and listen");
        warning->accept();
        listen->click();
        QCOMPARE(launches, 2);
        QVERIFY(!listen->isEnabled());

        bool choseChrome = false;
        QTimer::singleShot(0, &pane, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto* source = dialog->findChild<QComboBox*>("captionSource");
            if (source) {
                source->setCurrentText("Chrome Live Caption");
                choseChrome = source->currentIndex() == 1;
            }
            dialog->accept();
        });
        preferences->click();
        QVERIFY(choseChrome);
        const int checksBeforeChrome = windowChecks;
        windowOpen = true;
        QTest::qWait(350);
        QVERIFY(listen->isEnabled());
        QCOMPARE(listen->accessibleName(), "Resume caption listening");
        listen->click();
        QCOMPARE(listen->accessibleName(), "Stop caption listening");
        listen->click();
        QCOMPARE(listen->accessibleName(), "Resume caption listening");
        QCOMPARE(launches, 2);
        QCOMPARE(windowChecks, checksBeforeChrome);
    }

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
                auto* arrows = dialog->findChild<QCheckBox*>("speakerVolumeArrowKeys");
                fieldsFound = source && theme && hotkey && arrows && arrows->isChecked();
                if (fieldsFound) {
                    source->setCurrentIndex(1);
                    theme->setCurrentIndex(1);
                    arrows->setChecked(false);
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
        QVERIFY(settings.speakerVolumeArrowKeys);
        QVERIFY(editSettings(true));
        QVERIFY(fieldsFound);
        settings.Save();
        Settings savedSettings;
        savedSettings.Load();
        QCOMPARE(savedSettings.captionSource, CaptionSourcePreference::ChromeLiveCaption);
        QVERIFY(!savedSettings.speakerVolumeArrowKeys);
        QCOMPARE(settings.theme, UiTheme::Light);
        QCOMPARE(settings.captionSource, CaptionSourcePreference::ChromeLiveCaption);
        QCOMPARE(settings.hotkeyVirtualKey, unsigned(VK_F8));
        QCOMPARE(settings.hotkeyModifiers, unsigned(MOD_CONTROL | MOD_ALT));
        settings.hotkeyVirtualKey = 0;

        QSettings layout(temp.filePath("layout.ini"), QSettings::IniFormat);
        MeetingHandlerWindow window(settings, layout);
        QVERIFY(window.captionsReady());
        window.resize(1200, 640);
        showWithPingFixture(window);
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
        settings.captionSource = CaptionSourcePreference::ChromeLiveCaption;
        settings.transcriptPath = temp.filePath("captions.txt").toStdWString();
        settings.hotkeyVirtualKey = 0;
        settings.sidePanelWidth = 273;
        QPointer<CaptionView> formerView;
        int savedMeetingsWidth = 0;
        {
            MeetingHandlerWindow window(settings, layout);
            QVERIFY(window.captionsReady());
            window.resize(1200, 640);
            showWithPingFixture(window);
            window.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto* workspace = window.findChild<QWidget*>("workspace");
            QVERIFY(workspace);
            verifyColumnsFit(workspace);
            QVERIFY(!QIcon(":/icons/meeting-handler.ico").pixmap(16, 16).isNull());
            auto* captions = workspace->findChild<CaptionPane*>();
            auto* columns = workspace->findChild<QSplitter*>("workspaceColumns");
            QCOMPARE(columns->sizes().first(), settings.sidePanelWidth);
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
            QTest::keyClick(spacing, Qt::Key_Tab);
            QCOMPARE(QApplication::focusWidget(), view);
            toggleLog->setFocus();
            for (int i = 0; i < 12 && !controller->isAncestorOf(QApplication::focusWidget()); ++i)
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
            QCOMPARE(collapse->text(), QStringLiteral("\u25B6"));
            QVERIFY(!window.findChild<QWidget*>("pageRoot")->isVisible());
            QVERIFY(collapse->isVisible());
            QCOMPARE(boundsIn(controller->findChild<QWidget*>("collapseBar"), controller).top(), 0);
            QTRY_VERIFY(controller->width() < expandedWidths.at(1));
            QCOMPARE(columns->sizes().first(), expandedWidths.first());
            QCOMPARE(captions->width(), expandedWidths.last() + expandedWidths.at(1) - controller->width());
            QCOMPARE(window.size(), expandedWindow);
            QTest::mouseClick(collapse, Qt::LeftButton);
            QTRY_VERIFY(!controller->meetingsCollapsed());
            QVERIFY(collapse->text().contains(QStringLiteral("Meetings (0/0)")));
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
                auto* theme = dialog->findChild<QComboBox*>("captionTheme");
                if (!theme) { dialog->reject(); return; }
                theme->setCurrentIndex(static_cast<int>(UiTheme::Light));
                dialog->accept();
            });
            preferences->click();
            QVERIFY(foundSettings);
            QVERIFY(window.isEnabled());
            QCOMPARE(captions->palette().color(QPalette::AlternateBase), QColor(246, 246, 246));
            verifyColumnsFit(workspace);

            window.resize(window.minimumWidth(), window.height());
            columns->setSizes({950, 324, 100});
            QCoreApplication::processEvents();
            verifyColumnsFit(workspace);
            auto* mic = captions->findChild<QSlider*>("microphoneVolume");
            QVERIFY(mic);
            QVERIFY(boundsIn(mic, captions).top() > boundsIn(send, captions).bottom());
            QVERIFY(boundsIn(spacing, captions).bottom() < boundsIn(view, captions).top());

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
            auto* pingToggle = window.findChild<QToolButton*>("showPingStatus");
            QVERIFY(pingToggle && pingToggle->isChecked());
            pingToggle->click();

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
        showWithPingFixture(reopened);
        QVERIFY(QTest::qWaitForWindowExposed(&reopened));
        verifyColumnsFit(workspace);
        auto* controller = workspace->findChild<MeetingControllerPane*>();
        QVERIFY(controller->meetingsCollapsed());
        controller->setMeetingsCollapsed(false);
        QTRY_COMPARE(controller->width(), savedMeetingsWidth);
        QCOMPARE(workspace->findChild<CaptionView*>()->viewport()->grab(QRect(0, 0, 8, 8)).toImage().pixelColor(1, 1),
                 QColor(Qt::white));
        reopened.showMaximized();
        QTRY_VERIFY(reopened.isMaximized());
        QCoreApplication::processEvents();
        verifyColumnsFit(workspace);
    }
};

int main(int argc, char** argv) {
    if (argc == 2 && QByteArrayView(argv[1]) == "--ping-stream-fixture") {
        const HANDLE pipe = ::GetStdHandle(STD_OUTPUT_HANDLE);
        const QByteArrayView chunks[] = {
            "\r\nPinging google.com [192.0.2.1] with 32 bytes of data:\r\n"
            "Request timed out.\r\nReply from 192.0.2.1: byt",
            "es=32 time=17ms TTL=117\r\n",
            "Reply from 192.0.2.1: bytes=32 time<1ms TTL=128\r\n"
            "Antwort von 192.0.2.1: Bytes=64 Zeit=23 ms TTL=59\r\n",
            "Reply from 2001:db8::1: time=9ms \r\n"
            "Reply from 192.0.2.2: Destination host unreachable.\r\n"
            "\r\nPing statistics for 192.0.2.1:\r\n"
            "    Packets: Sent = 6, Received = 4, Lost = 2 (33% loss),\r\n"
            "Approximate round trip times in milli-seconds:\r\n"
            "    Minimum = 0ms, Maximum = 23ms, Average = 12ms\r\n"
        };
        for (const auto chunk : chunks) {
            DWORD written = 0;
            if (!::WriteFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &written, nullptr)) return 1;
            ::Sleep(75);
        }
        ::Sleep(7500); // Keep the child alive so the test can stop it through the sidebar.
        return 0;
    }
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
