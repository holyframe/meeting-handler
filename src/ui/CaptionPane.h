#pragma once

#include <QAbstractNativeEventFilter>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include "captions/CaptureEngine.h"
#include "captions/Settings.h"
#include "windows/ActiveWindowFollower.h"
#include "windows/WebInputPicker.h"
#include "windows/WindowsLiveCaptions.h"

class CaptionTargetList;
class CaptionView;
class QBoxLayout;
class QCheckBox;
class QComboBox;
class QSplitter;
class QToolButton;

class CaptionPane final : public QWidget, private QAbstractNativeEventFilter {
    Q_OBJECT
public:
    CaptionPane(QWidget* targetSidebar, QWidget* dragStrip, QWidget* parent,
                windows_captions::Backend windowsCaptions = windows_captions::systemBackend());
    ~CaptionPane() override;
    bool initialize(const Settings& settings);
    void setLogVisible(bool visible);
    void setTargetSidebarWidth(int width) { m_settings.sidePanelWidth = width; }

signals:
    void logLine(const QString& text);
    void logToggled(bool visible);

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void createControls();
    void restoreToolbarHeight();
    void ApplyTheme();
    void ApplyTypography();
    void UpdateHotkeyRegistration();
    void QueueHotkeySend();
    bool HotkeyChordReleased() const;
    void UpdateWindowTitle();
    void SetStatus(const std::wstring& text);
    void OnSend();
    void OnSave();
    void OnClear();
    void OnToggleListening();
    void UpdateListeningButton();
    void RequestWindowsCaptions(bool enabled);
    void ShowWindowsCaptionsState(bool enabled);
    void OnSettings();
    void OnCopy();
    void MaybeCopyRealtime();
    void discardPendingCaptions();
    void BeginWindowPick();
    void UpdateWindowPick(POINT point, bool forceRefresh = false);
    void FinishWindowPick(bool accept);
    void ShowWindowPickStatus(WebInputPickState state);
    void UpdatePickOutline(HWND window);

    WebInputPicker m_webInputPicker;
    CaptureEngine m_engine;
    Settings m_settings;
    ActiveWindowFollower m_activeWindowFollower;
    HWND m_frame = nullptr;
    bool m_initialized = false;
    bool m_restoreToolbar = true;
    bool m_hotkeyRegistered = false;
    bool m_windowPickDrag = false;
    bool m_windowPickUpdating = false;
    WebInputPickState m_windowPickState = WebInputPickState::NoWindow;
    QTimer m_followTimer, m_pickTimer, m_hotkeyTimer, m_presentTimer;
    windows_captions::Backend m_windowsCaptions;
    QFutureWatcher<windows_captions::Result> m_windowsCaptionsJob;
    std::stop_source m_cancelWindowsCaptions;
    QTimer m_windowsCaptionsMonitor;
    std::optional<bool> m_windowsCaptionsRequest;
    bool m_windowsCaptionsOn = false;
    bool m_windowsCaptionsWindowOpen = false;
    QElapsedTimer m_realtimeCopy;
    QWidget* m_dragStrip = nullptr;
    QWidget* m_pickOutline = nullptr;
    QWidget* m_targetSidebar = nullptr;
    QBoxLayout* m_toolbarGroups = nullptr;
    QSplitter* m_rows = nullptr;
    CaptionView* m_view = nullptr;
    QPointer<CaptionTargetList> m_targets;
    QCheckBox* m_pressEnterCheck = nullptr;
    QPointer<QToolButton> m_pickButton;
    QToolButton* m_listenButton = nullptr;
    QToolButton* m_windowsCaptionsButton = nullptr;
    QToolButton* m_logButton = nullptr;
    QComboBox* m_fontCombo = nullptr;
    QComboBox* m_sizeCombo = nullptr;
    QComboBox* m_spacingCombo = nullptr;
};
