#pragma once

#include <QAbstractNativeEventFilter>
#include <QElapsedTimer>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <optional>

#include "captions/CaptureEngine.h"
#include "captions/Settings.h"
#include "windows/ActiveWindowFollower.h"
#include "windows/WebInputPicker.h"

class AudioLevelControl;
class CaptionTargetList;
class CaptionView;
class WindowPickOutline;
class WindowsAudioLevels;
class QBoxLayout;
class QCheckBox;
class QComboBox;
class QShortcut;
class QSplitter;
class QToolButton;

class CaptionPane final : public QWidget, private QAbstractNativeEventFilter {
    Q_OBJECT
public:
    struct WindowsCaptionLauncher {
        std::function<bool()> windowOpen = CaptionSource::WindowsLiveCaptionsWindowOpen;
        std::function<bool()> launch = CaptionSource::LaunchWindowsLiveCaptions;
    };
    CaptionPane(QWidget* targetSidebar, QWidget* parent, WindowsCaptionLauncher windowsCaptions);
    CaptionPane(QWidget* targetSidebar, QWidget* parent)
        : CaptionPane(targetSidebar, parent, {}) {}
    ~CaptionPane() override;
    bool initialize(const Settings& settings);
    void setLogVisible(bool visible);
    void setTargetSidebarWidth(int width) { m_settings.sidePanelWidth = width; }

signals:
    void logLine(const QString& text);
    void logToggled(bool visible);
    void audioMonitoringChanged(bool enabled);
    void microphoneDeviceSelected(const QString& deviceId);
    void microphoneMuteToggleRequested();
    void speakerVolumeStepRequested(bool increase);

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override;
    void keyPressEvent(QKeyEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void createControls();
    QWidget* createFormattingBar();
    void connectAudioControls(AudioLevelControl* microphone, AudioLevelControl* speakers);
    void updateVolumeShortcuts();
    void restoreToolbarHeight();
    void ApplyTheme();
    void ApplyTypography();
    void UpdateHotkeyRegistration();
    void UnregisterHotkeys();
    void QueueHotkeySend();
    bool HotkeyChordReleased() const;
    void UpdateWindowTitle();
    void SetStatus(const std::wstring& text);
    void OnSend();
    void OnSave();
    void OnClear();
    void OnToggleListening();
    void SetListeningPaused(bool paused);
    void WindowsCaptionStartFailed(const QString& message);
    void UpdateListeningButton();
    void OnSettings();
    void OnCopy();
    void MaybeCopyRealtime();
    void discardPendingCaptions();
    void BeginWindowPick();
    void UpdateWindowPick(bool forceRefresh = false);
    void FinishWindowPick(bool accept);
    void ShowWindowPickStatus(WebInputPickState state);

    WebInputPicker m_webInputPicker;
    CaptureEngine m_engine;
    Settings m_settings;
    WindowsCaptionLauncher m_windowsCaptions;
    QTimer m_windowsCaptionStartTimer;
    QElapsedTimer m_windowsCaptionStartTime;
    bool m_listenButtonStarts = false;
    ActiveWindowFollower m_activeWindowFollower;
    HWND m_frame = nullptr;
    bool m_initialized = false;
    bool m_restoreToolbar = true;
    bool m_hotkeyRegistered = false;
    bool m_microphoneHotkeyRegistered = false;
    bool m_windowPickDrag = false;
    bool m_windowPickUpdating = false;
    WebInputPickState m_windowPickState = WebInputPickState::NoWindow;
    QTimer m_followTimer, m_pickTimer, m_hotkeyTimer, m_presentTimer;
    QElapsedTimer m_realtimeCopy;
    WindowPickOutline* m_pickOutline = nullptr;
    std::optional<POINT> m_pickReleasePoint;
    QWidget* m_targetSidebar = nullptr;
    QBoxLayout* m_toolbarGroups = nullptr;
    QThread m_audioThread;
    WindowsAudioLevels* m_audio = nullptr;
    QShortcut* m_volumeUp = nullptr;
    QShortcut* m_volumeDown = nullptr;
    QSplitter* m_rows = nullptr;
    CaptionView* m_view = nullptr;
    QPointer<CaptionTargetList> m_targets;
    QCheckBox* m_pressEnterCheck = nullptr;
    QPointer<QToolButton> m_pickButton;
    QToolButton* m_listenButton = nullptr;
    QToolButton* m_logButton = nullptr;
    QComboBox* m_fontCombo = nullptr;
    QComboBox* m_sizeCombo = nullptr;
    QComboBox* m_spacingCombo = nullptr;
};
