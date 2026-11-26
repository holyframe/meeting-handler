#include "CaptionPane.h"
#include "AudioLevelControl.h"
#include "CaptionTargetList.h"
#include "CaptionTheme.h"
#include "CaptionView.h"
#include "PingStatusPanel.h"
#include "SettingsDialog.h"
#include "WindowPickOutline.h"
#include "captions/CaptionSave.h"
#include "resource.h"

#include <windowsx.h>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyleHints>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <utility>

namespace {
class PingDividerButton final : public QToolButton {
public:
    explicit PingDividerButton(QWidget* parent) : QToolButton(parent) {
        setObjectName("showPingStatus");
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(caption_theme::splitterThickness);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setCheckable(true);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QToolButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().buttonText());
        const QPointF center = QRectF(rect()).center();
        for (int offset : {-5, 0, 5})
            painter.drawEllipse(center + QPointF(offset, 0), 1.2, 1.2);
    }
};

CaptionSourceChoice ToSourceChoice(CaptionSourcePreference source) {
    return source == CaptionSourcePreference::ChromeLiveCaption
               ? CaptionSourceChoice::Chrome
               : CaptionSourceChoice::WindowsLiveCaptions;
}

QToolButton* iconButton(QWidget* parent, const char* name, const QString& label, ushort glyph) {
    auto* button = new QToolButton(parent);
    button->setObjectName(name);
    button->setAccessibleName(label);
    button->setToolTip(label);
    button->setFont(QFont("Segoe MDL2 Assets", 11));
    button->setText(QChar(glyph));
    button->setFixedSize(23, 23);
    return button;
}

void selectValue(QComboBox* combo, const QString& value) {
    if (combo->findText(value) < 0) combo->addItem(value);
    combo->setCurrentText(value);
}

}

CaptionPane::CaptionPane(QWidget* targetSidebar, QWidget* parent,
                         WindowsCaptionLauncher windowsCaptions)
    : QWidget(parent), m_windowsCaptions(std::move(windowsCaptions)),
      m_targetSidebar(targetSidebar) {
    setObjectName("captionPane");
    setMinimumHeight(175);
    createControls();
    m_pickOutline = new WindowPickOutline(this);
    m_windowsCaptionStartTimer.setInterval(250);
    connect(&m_windowsCaptionStartTimer, &QTimer::timeout, this, [this] {
        if (m_windowsCaptions.windowOpen()) {
            m_windowsCaptionStartTimer.stop();
            SetListeningPaused(false);
        } else if (m_windowsCaptionStartTime.hasExpired(15000)) {
            WindowsCaptionStartFailed(tr("The Windows Live Captions window did not appear. "
                                         "Open Live Captions in Windows and try listening again."));
        }
    });
    m_followTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_followTimer, &QTimer::timeout, this, [this] { m_activeWindowFollower.Poll(); });
    connect(&m_pickTimer, &QTimer::timeout, this, [this] { UpdateWindowPick(); });
    m_hotkeyTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_hotkeyTimer, &QTimer::timeout, this, [this] {
        if (!HotkeyChordReleased()) return;
        m_hotkeyTimer.stop();
        OnSend();
    });
    m_presentTimer.setSingleShot(true);
    connect(&m_presentTimer, &QTimer::timeout, this, [this] {
        m_view->Present();
        MaybeCopyRealtime();
    });
    connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (m_settings.theme == UiTheme::System) ApplyTheme();
    });
}

CaptionPane::~CaptionPane() {
    UnregisterHotkeys();
    m_audioThread.quit();
    m_audioThread.wait();
    qApp->removeNativeEventFilter(this);
    m_engine.Stop();
    discardPendingCaptions();
    if (m_windowPickDrag) FinishWindowPick(false);
    // The sidebar and transcript are siblings. Release any surviving tiles before their picker.
    delete m_targets.data();
    m_activeWindowFollower.Stop();
    if (m_initialized) m_settings.Save();
}

void CaptionPane::createControls() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* sidebar = m_targetSidebar;
    sidebar->installEventFilter(this);
    sidebar->setAccessibleName(tr("Caption targets"));
    auto* sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(0);
    auto* pickerRow = new QHBoxLayout;
    pickerRow->addStrut(caption_theme::paneHeaderHeight);
    m_pickButton = iconButton(sidebar, "pickCaptionTarget", tr("Pick up window"), 0xE78B);
    m_pickButton->setFixedSize(caption_theme::headerControlHeight, caption_theme::headerControlHeight);
    m_pickButton->installEventFilter(this);
    pickerRow->addWidget(m_pickButton, 0, Qt::AlignCenter);
    sideLayout->addLayout(pickerRow);
    auto* targetScroll = new QScrollArea(sidebar);
    targetScroll->setObjectName("targetScroll");
    targetScroll->viewport()->setObjectName("targetViewport");
    targetScroll->setFrameShape(QFrame::NoFrame);
    targetScroll->setWidgetResizable(true);
    targetScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_targets = new CaptionTargetList(m_webInputPicker);
    targetScroll->setWidget(m_targets);
    sideLayout->addWidget(targetScroll, 3);
    auto* pingToggle = new PingDividerButton(sidebar);
    sideLayout->addWidget(pingToggle);
    auto* pingPanel = new PingStatusPanel(sidebar);
    sideLayout->addWidget(pingPanel, 1);
    const auto setPingExpanded = [pingToggle, pingPanel](bool expanded) {
        pingPanel->setVisible(expanded);
        const auto action = expanded ? tr("Hide ping status for google.com") : tr("Show ping status for google.com");
        pingToggle->setAccessibleName(action);
        pingToggle->setToolTip(action);
    };
    connect(pingToggle, &QToolButton::toggled, pingPanel, setPingExpanded);
    pingToggle->setChecked(true);
    auto* formatting = createFormattingBar();
    layout->addWidget(formatting);
    m_rows = new QSplitter(Qt::Vertical, this);
    m_rows->setObjectName("captionRows");
    m_rows->setChildrenCollapsible(false);
    m_rows->setHandleWidth(caption_theme::splitterThickness);
    layout->addWidget(m_rows, 1);
    m_view = new CaptionView;
    m_view->setMinimumHeight(80);
    m_rows->addWidget(m_view);
    setFocusProxy(m_view);
    auto* toolbar = new QWidget;
    toolbar->setObjectName("captionToolbar");
    auto* bottom = new QVBoxLayout(toolbar);
    bottom->setContentsMargins(8, 4, 8, 5);
    bottom->setSpacing(0);
    bottom->addStretch();
    m_toolbarGroups = new QBoxLayout(QBoxLayout::LeftToRight);
    m_toolbarGroups->setSpacing(6);
    bottom->addLayout(m_toolbarGroups);
    auto* sending = new QHBoxLayout;
    sending->setSpacing(6);
    m_toolbarGroups->addLayout(sending);
    auto* microphone = new AudioLevelControl(AudioEndpoint::Microphone, toolbar);
    sending->addWidget(microphone, 1);
    auto* send = new QPushButton(tr("Send"), toolbar);
    send->setObjectName("sendCaption");
    send->setFixedSize(92, 23);
    sending->addWidget(send);
    m_pressEnterCheck = new QCheckBox(tr("Press Enter"), toolbar);
    m_pressEnterCheck->setObjectName("pressEnter");
    m_pressEnterCheck->setMinimumWidth(96);
    sending->addWidget(m_pressEnterCheck);
    auto* playback = new QHBoxLayout;
    playback->setSpacing(6);
    m_toolbarGroups->addLayout(playback);
    auto* speakers = new AudioLevelControl(AudioEndpoint::Speakers, toolbar);
    playback->addWidget(speakers, 1);
    m_listenButton = iconButton(toolbar, "toggleListening", tr("Stop caption listening"), 0xE71A);
    auto* save = iconButton(toolbar, "saveCaptions", tr("Save captions"), 0xE74E);
    auto* clear = iconButton(toolbar, "clearCaptions", tr("Clear captions"), 0xE74D);
    auto* settings = iconButton(toolbar, "captionPreferences", tr("Settings"), 0xE713);
    m_logButton = iconButton(toolbar, "toggleActivityLog", tr("Activity log"), 0xE7BA);
    m_logButton->setCheckable(true);
    for (auto* button : {m_listenButton, save, clear, settings, m_logButton}) playback->addWidget(button);
    connectAudioControls(microphone, speakers);
    const auto margins = bottom->contentsMargins();
    const int toolbarMinWidth = qMax(sending->minimumSize().width(), playback->minimumSize().width())
                                + margins.left() + margins.right();
    setMinimumWidth(qMax(toolbarMinWidth, formatting->minimumSizeHint().width()));
    toolbar->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_rows->addWidget(toolbar);
    m_rows->setStretchFactor(0, 1);
    m_rows->setStretchFactor(1, 0);

    connect(m_rows, &QSplitter::splitterMoved, this, [this] { m_settings.bottomPanelHeight = m_rows->sizes().last(); });
    connect(send, &QPushButton::clicked, this, &CaptionPane::OnSend);
    connect(m_pressEnterCheck, &QCheckBox::toggled, this, [this](bool checked) { m_settings.pressEnter = checked; });
    connect(save, &QToolButton::clicked, this, &CaptionPane::OnSave);
    connect(clear, &QToolButton::clicked, this, &CaptionPane::OnClear);
    connect(settings, &QToolButton::clicked, this, &CaptionPane::OnSettings);
    connect(m_listenButton, &QToolButton::clicked, this, &CaptionPane::OnToggleListening);
    connect(m_logButton, &QToolButton::toggled, this, &CaptionPane::logToggled);
    connect(m_targets, &CaptionTargetList::selectionChanged, this, &CaptionPane::UpdateWindowTitle);
    connect(m_targets, &CaptionTargetList::statusChanged, this, &CaptionPane::logLine);
    connect(m_pickButton, &QToolButton::clicked, this, [this] {
        SetStatus(L"Drag this button onto a browser tab that contains an editable field.");
    });
    connect(m_view, &CaptionView::copyRequested, this, &CaptionPane::OnCopy);
    for (auto* box : {m_fontCombo, m_sizeCombo, m_spacingCombo}) {
        connect(box, &QComboBox::currentIndexChanged, this, [this] {
            if (!m_initialized) return;
            m_settings.fontFamily = m_fontCombo->currentText().toStdWString();
            m_settings.fontSizePt = m_sizeCombo->currentText().toInt();
            m_settings.lineSpacing = m_spacingCombo->currentText().toDouble();
            ApplyTypography();
        });
    }
}

QWidget* CaptionPane::createFormattingBar() {
    auto* bar = new QWidget(this);
    bar->setObjectName("captionFormattingBar");
    bar->setFixedHeight(caption_theme::paneHeaderHeight);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(8, caption_theme::headerVerticalMargin, 8, caption_theme::headerVerticalMargin);
    row->setSpacing(6);
    const auto combo = [&](const char* name, const QString& label, const QString& accessibleName) {
        auto* title = new QLabel(label, bar);
        auto* box = new QComboBox(bar);
        box->setObjectName(name);
        box->setAccessibleName(accessibleName);
        box->setToolTip(accessibleName);
        box->setFixedHeight(caption_theme::headerControlHeight);
        title->setBuddy(box);
        row->addWidget(title);
        row->addWidget(box);
        return box;
    };
    m_fontCombo = combo("captionFont", tr("Font"), tr("Font"));
    m_fontCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_fontCombo->setMinimumContentsLength(13);
    m_fontCombo->setMinimumWidth(84);
    m_fontCombo->setMaximumWidth(160);
    m_fontCombo->addItems(QFontDatabase::families());
    m_sizeCombo = combo("captionSize", tr("Size"), tr("Font size"));
    m_sizeCombo->setFixedWidth(52);
    for (int size : {8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 22, 24, 28, 32, 36, 42, 48})
        m_sizeCombo->addItem(QString::number(size));
    m_spacingCombo = combo("captionSpacing", tr("Line spacing"), tr("Line spacing"));
    m_spacingCombo->setFixedWidth(58);
    for (double spacing : {1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.75, 2.0, 2.5, 3.0})
        m_spacingCombo->addItem(QString::number(spacing));
    row->addStretch();
    return bar;
}

void CaptionPane::connectAudioControls(AudioLevelControl* microphone, AudioLevelControl* speakers) {
    m_audio = new WindowsAudioLevels;
    m_audio->moveToThread(&m_audioThread);
    connect(&m_audioThread, &QThread::finished, m_audio, &QObject::deleteLater);
    connect(this, &CaptionPane::audioMonitoringChanged, m_audio, &WindowsAudioLevels::setMonitoring);
    connect(this, &CaptionPane::microphoneDeviceSelected, m_audio, &WindowsAudioLevels::selectMicrophone);
    connect(this, &CaptionPane::microphoneMuteToggleRequested, m_audio, &WindowsAudioLevels::toggleMicrophoneMute);
    connect(this, &CaptionPane::speakerVolumeStepRequested, m_audio, &WindowsAudioLevels::stepSpeakerVolume);
    const auto volumeShortcut = [this](Qt::Key key) {
        return new QShortcut(QKeySequence(key), this, [this, key] {
            emit speakerVolumeStepRequested(key == Qt::Key_Up);
        }, Qt::WindowShortcut);
    };
    m_volumeUp = volumeShortcut(Qt::Key_Up);
    m_volumeDown = volumeShortcut(Qt::Key_Down);
    connect(qApp, &QApplication::focusChanged, this, &CaptionPane::updateVolumeShortcuts);
    connect(m_audio, &WindowsAudioLevels::writeFailed, this, [this, microphone, speakers](AudioEndpoint endpoint) {
        const QString message = endpoint == AudioEndpoint::Microphone
            ? tr("Windows could not apply the microphone change.") : tr("Windows could not apply the sound output change.");
        emit logLine(message);
        auto* control = endpoint == AudioEndpoint::Microphone ? microphone : speakers;
        if (control->isVisible()) QToolTip::showText(control->mapToGlobal(QPoint()), message, control);
    });
    for (auto* control : {microphone, speakers}) {
        connect(m_audio, &WindowsAudioLevels::levelsChanged, control, &AudioLevelControl::setLevels);
        connect(control, &AudioLevelControl::volumeRequested, m_audio, &WindowsAudioLevels::setVolume);
        connect(control, &AudioLevelControl::muteRequested, m_audio, &WindowsAudioLevels::setMuted);
    }
    m_audioThread.setObjectName("Windows audio levels");
    m_audioThread.start();
}

void CaptionPane::updateVolumeShortcuts() {
    auto* focus = QApplication::focusWidget();
    const bool editingOrNavigating = qobject_cast<QComboBox*>(focus) || qobject_cast<QAbstractSlider*>(focus)
        || qobject_cast<QAbstractItemView*>(focus) || qobject_cast<QLineEdit*>(focus)
        || qobject_cast<QPlainTextEdit*>(focus);
    const bool enabled = m_settings.speakerVolumeArrowKeys && !editingOrNavigating;
    m_volumeUp->setEnabled(enabled);
    m_volumeDown->setEnabled(enabled);
}

bool CaptionPane::initialize(const Settings& settings) {
    if (m_initialized) return true;
    m_settings = settings;
    emit microphoneDeviceSelected(QString::fromStdWString(settings.microphoneDeviceId));
    updateVolumeShortcuts();
    m_frame = reinterpret_cast<HWND>(window()->winId());
    if (!m_frame) return false;
    m_pressEnterCheck->setChecked(settings.pressEnter);
    selectValue(m_fontCombo, QString::fromStdWString(settings.fontFamily));
    selectValue(m_sizeCombo, QString::number(settings.fontSizePt));
    selectValue(m_spacingCombo, QString::number(settings.lineSpacing));
    ApplyTheme();
    ApplyTypography();
    UpdateHotkeyRegistration();
    qApp->installNativeEventFilter(this);
    m_followTimer.start(10);
    m_initialized = true;
    if (!m_engine.Start(this, settings.ResolvedTranscriptPath(), settings.pollIntervalMs, ToSourceChoice(settings.captionSource)))
        SetStatus(L"Could not start caption capture.");
    else SetStatus(L"Saving transcript to " + settings.ResolvedTranscriptPath());
    UpdateListeningButton();
    return true;
}

void CaptionPane::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    emit audioMonitoringChanged(true);
    if (!m_restoreToolbar || !m_initialized) return;
    layout()->activate();
    restoreToolbarHeight();
    m_restoreToolbar = false;
}

void CaptionPane::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    emit audioMonitoringChanged(false);
}

void CaptionPane::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    auto* toolbar = m_rows->widget(1);
    const auto margins = toolbar->layout()->contentsMargins();
    const int fullRow = m_toolbarGroups->itemAt(0)->minimumSize().width()
        + m_toolbarGroups->itemAt(1)->minimumSize().width() + m_toolbarGroups->spacing()
        + margins.left() + margins.right();
    const auto direction = width() < fullRow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    if (direction == m_toolbarGroups->direction()) return;
    m_toolbarGroups->setDirection(direction);
    toolbar->setMinimumHeight(m_toolbarGroups->minimumSize().height() + margins.top() + margins.bottom());
    if (m_initialized) restoreToolbarHeight();
}

void CaptionPane::restoreToolbarHeight() {
    m_rows->setSizes({std::max(1, m_rows->height() - m_rows->handleWidth() - m_settings.bottomPanelHeight), m_settings.bottomPanelHeight});
}

void CaptionPane::ApplyTheme() {
    caption_theme::apply(this, m_settings.theme);
    m_view->setPalette(palette());
    m_targetSidebar->setStyleSheet(styleSheet());
    m_targetSidebar->setPalette(palette());
    emit headerPaletteChanged(palette());
}

void CaptionPane::ApplyTypography() {
    m_view->SetTypography(m_settings.fontFamily, m_settings.fontSizePt, m_settings.lineSpacing);
}

void CaptionPane::setLogVisible(bool visible) {
    const QSignalBlocker guard(m_logButton);
    m_logButton->setChecked(visible);
}

void CaptionPane::SetStatus(const std::wstring& text) {
    if (!text.empty()) emit logLine(QString::fromStdWString(text));
}

void CaptionPane::UpdateWindowTitle() {
    const QString target = m_webInputPicker.SelectedWindow()
        ? (m_webInputPicker.SelectedName().empty() ? tr("Untitled target") : QString::fromStdWString(m_webInputPicker.SelectedName()))
        : tr("No target selected");
    setWindowTitle(tr("Live Caption App (Hotkey - %1) : %2")
        .arg(captionHotkeyName(m_settings.hotkeyModifiers, m_settings.hotkeyVirtualKey), target));
}

bool CaptionPane::event(QEvent* event) {
    if (event->type() == CaptionUpdate::eventType()) {
        auto* change = static_cast<CaptionUpdate*>(event);
        m_view->QueueUpdate(change->firstDirtyLine, std::move(change->lines));
        if (!m_presentTimer.isActive()) m_presentTimer.start(0);
        return true;
    }
    if (event->type() == CaptionStatus::eventType()) {
        if (!m_windowsCaptionStartTimer.isActive()) {
            if (!m_windowPickDrag) SetStatus(static_cast<CaptionStatus*>(event)->text);
            UpdateListeningButton();
        }
        return true;
    }
    return QWidget::event(event);
}

void CaptionPane::discardPendingCaptions() {
    m_presentTimer.stop();
    QCoreApplication::removePostedEvents(this, CaptionUpdate::eventType());
    QCoreApplication::removePostedEvents(this, CaptionStatus::eventType());
}

void CaptionPane::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        if (m_windowPickDrag) FinishWindowPick(false);
        else if (!m_targets->CancelRemoval()) m_view->ClearSelection();
    } else if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && m_settings.pressEnter) {
        OnSend();
    } else return QWidget::keyPressEvent(event);
    event->accept();
}

void CaptionPane::wheelEvent(QWheelEvent* event) {
    auto* bar = m_view->verticalScrollBar();
    const int delta = event->pixelDelta().isNull()
        ? qRound(event->angleDelta().y() / 120.0 * QApplication::wheelScrollLines() * bar->singleStep())
        : event->pixelDelta().y();
    bar->setValue(bar->value() - delta);
    event->accept();
}

void CaptionPane::UnregisterHotkeys() {
    m_hotkeyTimer.stop();
    if (m_hotkeyRegistered) ::UnregisterHotKey(m_frame, HOTKEY_SEND);
    if (m_microphoneHotkeyRegistered) ::UnregisterHotKey(m_frame, HOTKEY_MICROPHONE_MUTE);
    m_hotkeyRegistered = false;
    m_microphoneHotkeyRegistered = false;
}

void CaptionPane::UpdateHotkeyRegistration() {
    m_hotkeyRegistered = m_settings.hotkeyVirtualKey &&
        ::RegisterHotKey(m_frame, HOTKEY_SEND, m_settings.hotkeyModifiers | MOD_NOREPEAT, m_settings.hotkeyVirtualKey);
    m_microphoneHotkeyRegistered = m_settings.microphoneHotkeyVirtualKey &&
        ::RegisterHotKey(m_frame, HOTKEY_MICROPHONE_MUTE,
                         m_settings.microphoneHotkeyModifiers | MOD_NOREPEAT, m_settings.microphoneHotkeyVirtualKey);
    UpdateWindowTitle();
    if (m_settings.hotkeyVirtualKey && !m_hotkeyRegistered)
        SetStatus(L"The Send hotkey is already in use by another app.");
    if (m_settings.microphoneHotkeyVirtualKey && !m_microphoneHotkeyRegistered)
        SetStatus(L"The microphone mute shortcut is unavailable. Choose another shortcut in Settings.");
}

bool CaptionPane::nativeEventFilter(const QByteArray&, void* message, qintptr* result) {
    const auto* msg = static_cast<MSG*>(message);
    if (m_windowPickDrag && msg->message == WM_LBUTTONUP && ::GetAncestor(msg->hwnd, GA_ROOT) == m_frame) {
        // Preserve the release location even if the pointer moves before Qt delivers the event.
        POINT point{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        if (::ClientToScreen(msg->hwnd, &point)) m_pickReleasePoint = point;
    }
    if (msg->hwnd != m_frame) return false;
    if (msg->message == WM_HOTKEY) {
        if (msg->wParam == HOTKEY_SEND) QueueHotkeySend();
        else if (msg->wParam == HOTKEY_MICROPHONE_MUTE && m_microphoneHotkeyRegistered)
            emit microphoneMuteToggleRequested();
        else return false;
        if (result) *result = 0;
        return true;
    }
    // Keep an existing selection when the first click merely activates this window.
    if (result && msg->message == WM_MOUSEACTIVATE && m_view->HasSelection() && ::GetForegroundWindow() != m_frame &&
        m_view->viewport()->rect().contains(m_view->viewport()->mapFromGlobal(QCursor::pos()))) {
        *result = MA_ACTIVATEANDEAT;
        return true;
    }
    return false;
}

void CaptionPane::QueueHotkeySend() {
    if (m_hotkeyTimer.isActive()) return;
    if (m_view->HasSelection()) m_webInputPicker.RaiseSelectedWindow();
    if (HotkeyChordReleased()) OnSend();
    else m_hotkeyTimer.start(10);
}

bool CaptionPane::HotkeyChordReleased() const {
    const auto released = [](int virtualKey) {
        return (::GetAsyncKeyState(virtualKey) & 0x8000) == 0;
    };

    if (m_settings.hotkeyVirtualKey != 0 &&
        !released(static_cast<int>(m_settings.hotkeyVirtualKey))) {
        return false;
    }
    const unsigned modifiers = m_settings.hotkeyModifiers;
    if ((modifiers & MOD_SHIFT) != 0 && !released(VK_SHIFT)) return false;
    if ((modifiers & MOD_CONTROL) != 0 && !released(VK_CONTROL)) return false;
    if ((modifiers & MOD_ALT) != 0 && !released(VK_MENU)) return false;
    if ((modifiers & MOD_WIN) != 0 && (!released(VK_LWIN) || !released(VK_RWIN))) return false;
    return true;
}

void CaptionPane::OnSend() {
    if (!m_view->HasSelection()) {
        SetStatus(L"Select caption text before sending.");
        return;
    }
    if (!m_webInputPicker.SelectedWindow()) {
        SetStatus(L"Pick a web tab before sending.");
        return;
    }

    const bool pressEnter = m_settings.pressEnter;

    std::wstring detail;
    const auto result = m_webInputPicker.SendText(m_view->SelectedText(), pressEnter, detail);
    if (result == WebInputSendResult::Failed) {
        SetStatus(detail);
        return;
    }

    // A verified insertion completes the sticky tail selection. An unconfirmed
    // Enter also clears it because submission may already have happened; other
    // failures keep it intact so retrying remains convenient.
    m_view->ClearSelection();

    if (result == WebInputSendResult::SubmissionUnconfirmed) {
        SetStatus(detail);
        return;
    }

    std::wstring status =
        result == WebInputSendResult::Submitted ? L"Sent selection to "
                                                : L"Inserted selection in ";
    status += m_webInputPicker.SelectedName();
    status += result == WebInputSendResult::Submitted ? L" and confirmed submission." : L".";
    SetStatus(status);
}

void CaptionPane::OnSave() {
    if (m_view->Empty()) {
        SetStatus(L"There are no captions to save.");
        return;
    }
    const QDir folder(QString::fromStdWString(m_settings.ResolvedSaveFolder()));
    if (!folder.exists() && !QDir().mkpath(folder.absolutePath())) {
        SetStatus(L"Could not create or access the configured caption save folder.");
        return;
    }
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    QFileDialog dialog(this);
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    caption_theme::apply(&dialog, m_settings.theme);
    dialog.setWindowTitle(tr("Save captions"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDirectory(folder);
    dialog.setNameFilters({tr("Text files (*.txt)"), tr("All files (*)")});
    dialog.setDefaultSuffix("txt");
    dialog.selectFile(QString::fromStdWString(caption_save::BuildFileName(now, m_webInputPicker.SelectedName())));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
    const QString path = dialog.selectedFiles().first();
    // Include revisions received while the dialog was open. Later deliveries belong
    // to the next batch, including those queued during the file write.
    QCoreApplication::sendPostedEvents(this, CaptionUpdate::eventType());
    QSaveFile file(path);
    const auto bytes = QString::fromStdWString(m_view->FullText()).toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write("\xef\xbb\xbf", 3) != 3 ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        SetStatus(L"Could not finish writing the caption file.");
        return;
    }
    m_presentTimer.stop();
    m_view->Clear();
    emit logLine(tr("Saved captions to %1 and cleared the caption pane.").arg(path));
}

void CaptionPane::OnClear() {
    discardPendingCaptions();
    m_view->Clear();
    SetStatus(L"Caption pane cleared.");
}

void CaptionPane::OnToggleListening() {
    if (m_windowsCaptionStartTimer.isActive()) return;
    if (m_settings.captionSource == CaptionSourcePreference::WindowsLiveCaptions
        && !m_windowsCaptions.windowOpen()) {
        m_engine.SetPaused(true);
        if (!m_windowsCaptions.launch()) {
            WindowsCaptionStartFailed(tr("Windows Live Captions could not be opened. "
                                         "Check that Live Captions is available in Windows Settings and try again."));
            return;
        }
        m_windowsCaptionStartTime.start();
        m_windowsCaptionStartTimer.start();
        UpdateListeningButton();
        SetStatus(L"Starting Windows Live Captions\u2026");
        return;
    }
    SetListeningPaused(!m_listenButtonStarts);
}

void CaptionPane::SetListeningPaused(bool paused) {
    m_engine.SetPaused(paused);
    UpdateListeningButton();
    SetStatus(paused ? L"Caption listening stopped. Existing text is preserved."
                     : L"Caption listening resumed.");
}

void CaptionPane::WindowsCaptionStartFailed(const QString& message) {
    m_windowsCaptionStartTimer.stop();
    UpdateListeningButton();
    SetStatus(message.toStdWString());
    auto* warning = new QMessageBox(QMessageBox::Warning, tr("Windows Live Captions"),
                                    message, QMessageBox::Ok, this);
    warning->setAttribute(Qt::WA_DeleteOnClose);
    warning->open();
}

void CaptionPane::UpdateListeningButton() {
    const bool starting = m_windowsCaptionStartTimer.isActive();
    const bool needsWindows = !starting && m_settings.captionSource == CaptionSourcePreference::WindowsLiveCaptions
                              && !m_windowsCaptions.windowOpen();
    m_listenButtonStarts = needsWindows || m_engine.IsPaused();
    QString action = m_listenButtonStarts ? tr("Resume caption listening") : tr("Stop caption listening");
    if (starting) action = tr("Starting Windows Live Captions\u2026");
    else if (needsWindows) action = tr("Start Windows Live Captions and listen");
    m_listenButton->setEnabled(m_engine.IsRunning() && !starting);
    m_listenButton->setText(QChar(m_listenButtonStarts ? 0xE768 : 0xE71A));
    m_listenButton->setAccessibleName(action);
    m_listenButton->setToolTip(action);
}

void CaptionPane::OnSettings() {
    const auto previous = m_settings.captionSource;
    const auto previousMicrophone = m_settings.microphoneDeviceId;
    UnregisterHotkeys();
    const bool accepted = ShowSettingsDialog(window(), m_settings, *m_audio);
    if (m_settings.microphoneDeviceId != previousMicrophone)
        emit microphoneDeviceSelected(QString::fromStdWString(m_settings.microphoneDeviceId));
    UpdateHotkeyRegistration();
    if (!accepted) return;
    updateVolumeShortcuts();
    m_settings.Save();
    ApplyTheme();
    if (previous != m_settings.captionSource) {
        m_windowsCaptionStartTimer.stop();
        m_engine.Stop();
        discardPendingCaptions();
        m_view->Clear();
        const bool started = m_engine.Start(this, m_settings.ResolvedTranscriptPath(), m_settings.pollIntervalMs,
                                            ToSourceChoice(m_settings.captionSource));
        UpdateListeningButton();
        if (!started) {
            SetStatus(L"Could not start caption capture.");
            return;
        }
    }
    SetStatus(L"Settings saved.");
}

void CaptionPane::OnCopy() {
    if (m_view->Empty()) { SetStatus(L"Nothing to copy yet."); return; }
    const bool selection = m_view->HasSelection();
    qApp->clipboard()->setText(QString::fromStdWString(selection ? m_view->SelectedText() : m_view->FullText()));
    SetStatus(selection ? L"Selection copied to the clipboard." : L"Whole transcript copied to the clipboard.");
}

void CaptionPane::MaybeCopyRealtime() {
    if (!m_settings.copyRealtime || ::GetForegroundWindow() != m_frame || m_view->HasSelection()) return;
    if (m_realtimeCopy.isValid() && m_realtimeCopy.elapsed() < 600) return;
    m_realtimeCopy.start();
    qApp->clipboard()->setText(QString::fromStdWString(m_view->FullText()));
}

bool CaptionPane::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_targetSidebar && event->type() == QEvent::KeyPress) {
        keyPressEvent(static_cast<QKeyEvent*>(event));
        return event->isAccepted();
    }
    if (watched == m_targetSidebar && event->type() == QEvent::Wheel) {
        wheelEvent(static_cast<QWheelEvent*>(event));
        return event->isAccepted();
    }
    if (watched != m_pickButton) return false;
    switch (event->type()) {
    case QEvent::MouseButtonPress:
        if (static_cast<QMouseEvent*>(event)->button() != Qt::LeftButton) return false;
        BeginWindowPick();
        return true;
    case QEvent::MouseMove:
        if (!m_windowPickDrag) return false;
        UpdateWindowPick();
        return true;
    case QEvent::MouseButtonRelease:
        if (!m_windowPickDrag || static_cast<QMouseEvent*>(event)->button() != Qt::LeftButton) return false;
        m_pickOutline->hide();
        UpdateWindowPick(true);
        FinishWindowPick(true);
        return true;
    case QEvent::UngrabMouse:
    case QEvent::Hide:
        if (m_windowPickDrag) FinishWindowPick(false);
        return false;
    default: return false;
    }
}

void CaptionPane::BeginWindowPick() {
    if (m_windowPickDrag || m_windowPickUpdating) return;
    m_webInputPicker.ResetCandidate();
    m_pickReleasePoint.reset();
    m_windowPickState = WebInputPickState::NoWindow;
    m_windowPickDrag = true;
    m_pickButton->setFocus();
    m_pickButton->setDown(true);
    m_pickButton->grabMouse();
    QApplication::setOverrideCursor(Qt::ForbiddenCursor);
    m_pickTimer.start(50);
    SetStatus(L"Drag over a browser tab. Release when the cursor changes to a crosshair.");
}

void CaptionPane::UpdateWindowPick(bool forceRefresh) {
    if (!m_windowPickDrag || m_windowPickUpdating) return;
    POINT point{};
    if (forceRefresh && m_pickReleasePoint) {
        point = *m_pickReleasePoint;
    } else if (!::GetCursorPos(&point)) {
        m_windowPickState = WebInputPickState::NoWindow;
        m_pickOutline->hide();
        return;
    }
    QScopedValueRollback guard(m_windowPickUpdating, true);
    const auto state = forceRefresh ? m_webInputPicker.Inspect(point, m_frame, true)
                                    : m_webInputPicker.Preview(point, m_frame);
    if (!m_windowPickDrag) { m_webInputPicker.ResetCandidate(); return; }
    m_windowPickState = state;
    const bool valid = IsPickableState(state);
    QApplication::changeOverrideCursor(valid ? Qt::CrossCursor : Qt::ForbiddenCursor);
    if (valid && !forceRefresh) m_pickOutline->showAround(m_webInputPicker.CandidateWindow());
    else m_pickOutline->hide();
    ShowWindowPickStatus(state);
}

void CaptionPane::FinishWindowPick(bool accept) {
    if (!m_windowPickDrag) return;
    m_windowPickDrag = false;
    m_pickTimer.stop();
    m_pickOutline->hide();
    if (m_pickButton) {
        if (mouseGrabber() == m_pickButton) m_pickButton->releaseMouse();
        m_pickButton->setDown(false);
    }
    QApplication::restoreOverrideCursor();
    const int before = m_webInputPicker.PickedCount();
    if (accept && !m_windowPickUpdating && IsPickableState(m_windowPickState) && m_webInputPicker.CommitCandidate()) {
        m_targets->Refresh();
        UpdateWindowTitle();
        std::wstring name = m_webInputPicker.CandidateName();
        if (name.empty()) name = m_webInputPicker.SelectedName();
        SetStatus((m_webInputPicker.PickedCount() > before ? L"Added web tab: " : L"Picked web tab: ") + name + L".");
    } else if (!accept) SetStatus(L"Window picking cancelled.");
    else ShowWindowPickStatus(m_windowPickState);
    if (!m_windowPickUpdating) m_webInputPicker.ResetCandidate();
}

void CaptionPane::ShowWindowPickStatus(WebInputPickState state) {
    switch (state) {
        case WebInputPickState::Checking:
            SetStatus(L"Checking this window for an editable web input...");
            return;
        case WebInputPickState::Valid: {
            std::wstring status = L"Release to pick: ";
            status += m_webInputPicker.CandidateName();
            status += L".";
            SetStatus(status);
            return;
        }
        case WebInputPickState::OwnWindow:
            SetStatus(L"This app cannot be selected.");
            return;
        case WebInputPickState::NoWebDocument:
            SetStatus(m_windowPickDrag
                          ? L"Drop unavailable: this browser does not expose an accessible web input."
                          : L"No accessible web input. Open the chat tab in a supported browser.");
            return;
        case WebInputPickState::NoEditableInput:
            SetStatus(L"Drop unavailable: no enabled editable web input. Open the chat tab "
                      L"and drag onto its message box.");
            return;
        default:
            SetStatus(L"Drop unavailable: point at a visible browser or WebView window.");
            return;
    }
}
