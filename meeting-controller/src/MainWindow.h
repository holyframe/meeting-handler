#pragma once

#include "MeetingBatch.h"

#include <QMainWindow>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

class QCloseEvent;
class QComboBox;
class QFrame;
class QGridLayout;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QResizeEvent;
class QScrollArea;
class QStackedWidget;
class QTabWidget;
class QThread;
class UiaWorker;

class MainWindow final : public QMainWindow
{
    Q_OBJECT
    friend class MeetingControllerTests;

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

signals:
    void scanRequested();
    void inspectRequested(quintptr hwndValue);
    void pickupRequested();
    void activationRequested(quintptr hwndValue,
                             const QString& tabRuntimeId,
                             const QString& tabName,
                             const QString& requestKey);
    void microphoneReadRequested(quintptr hwndValue,
                                 const QString& tabRuntimeId,
                                 const QString& tabName,
                                 const QString& requestKey);
    void microphoneSetRequested(quintptr hwndValue,
                                const QString& tabRuntimeId,
                                const QString& tabName,
                                bool muted,
                                const QString& requestKey);
    void audioReadRequested(quintptr hwndValue,
                            const QString& tabRuntimeId,
                            const QString& tabName,
                            const QString& requestKey);
    void audioSetRequested(quintptr hwndValue,
                           const QString& tabRuntimeId,
                           const QString& tabName,
                           bool connected,
                           const QString& requestKey);
    void prepareImportedMeetingsRequested(const QJsonArray& targets);

private slots:
    void onInitialized(bool success, const QString& message);
    void onChromeWindowsFound(const QJsonArray& windows);
    void onInspectionFinished(const QJsonObject& result);
    void onChromeTabsListed(const QJsonObject& result);
    void onMeetingActivated(const QJsonObject& result);
    void onMicrophoneRead(const QJsonObject& result);
    void onMicrophoneSet(const QJsonObject& result);
    void onAudioConnectionRead(const QJsonObject& result);
    void onAudioConnectionSet(const QJsonObject& result);
    void onBatchMeetingFinished(const QJsonObject& result);
    void onBatchFinished(int skipped);
    void onImportedMeetingsPrepared(const QJsonArray& results);
    void refreshChromeWindows();
    void inspectSelectedWindow();
    void requestMeetingTabImport();
    void requestMuteAll();
    void requestSoundOffAll();
    void requestRefreshConnections();

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class Microphone
    {
        Unknown,
        Muted,
        Unmuted,
        AudioNotConnected
    };
    enum class Sound { Unknown, On, Off };

    struct ManagedMeeting
    {
        QString windowHandle;
        qint64 processId = 0;
        QString tabRuntimeId;
        QString title;
        QString chromeTabName;
        QString host;
        QString meetingId;
        QString profileName;
        int windowNumber = 0;
        Microphone microphone = Microphone::Unknown;
        Sound sound = Sound::Unknown;

        QString key() const { return windowHandle + '/' + tabRuntimeId; }
        QString automationName() const { return chromeTabName.isEmpty() ? title : chromeTabName; }
        quintptr handle() const
        {
            return static_cast<quintptr>(windowHandle.toULongLong(nullptr, 16));
        }
    };

    QWidget* createPageBar();
    QWidget* createDashboardPage();
    QWidget* createDiagnosticsPage();
    QWidget* createSettingsPage();
    QWidget* createAboutPage();

    QFrame* createMeetingCard(const ManagedMeeting& meeting);
    void refreshMeetingCards();
    void refreshBatchCard(const QString& previousKey, const ManagedMeeting& meeting);
    void updateMeetingCounts();
    void layoutMeetingCards();
    void removeMeeting(const QString& key);
    void moveMeeting(const QString& fromKey, const QString& toKey);
    void activateMeeting(const QString& key);
    void readMicrophone(const QString& key);
    void setMicrophone(const QString& key, bool muted);
    void readSound(const QString& key);
    void setSound(const QString& key, bool on);
    ManagedMeeting* findMeeting(const QString& key);
    void setMeetingBusy(bool busy, const QString& statusMessage);
    void startMeetingBatch(MeetingBatch::Operation operation);
    void applyMicrophoneResult(const QJsonObject& result, const QString& action);
    void applyAudioResult(const QJsonObject& result, const QString& action);
    void updateMeetingStates(ManagedMeeting& meeting, const QJsonObject& result);
    bool meetingCardIsActive(const ManagedMeeting& meeting) const;
    QFrame* meetingCardFor(QWidget* widget) const;
    QFrame* meetingCardAt(const QPoint& globalPos) const;
    void setCardFlag(QFrame* card, const char* name, bool on);
    void setDropHover(QFrame* card);
    void clearDrag();

    void navigateTo(int pageIndex);
    void appendLog(const QString& message);
    void updateUiaStatus(bool ready, const QString& message);
    quintptr selectedWindowHandle() const;
    void applyDarkProfessionalTheme();

    QThread* workerThread_ = nullptr;
    UiaWorker* worker_ = nullptr;
    MeetingBatch* batch_ = nullptr;
    MeetingBatch::Operation batchOperation_ = MeetingBatch::Operation::Refresh;
    int batchChanged_ = 0;
    int batchAlready_ = 0;
    QStringList batchFailures_;

    QWidget* dashboardPage_ = nullptr;
    QWidget* diagnosticsPage_ = nullptr;
    QWidget* settingsPage_ = nullptr;
    QWidget* aboutPage_ = nullptr;
    QStackedWidget* pageStack_ = nullptr;

    QPushButton* dashboardNavButton_ = nullptr;
    QPushButton* diagnosticsNavButton_ = nullptr;
    QPushButton* settingsNavButton_ = nullptr;
    QPushButton* aboutNavButton_ = nullptr;

    QPushButton* dashboardPickUpButton_ = nullptr;
    QPushButton* dashboardMuteAllButton_ = nullptr;
    QPushButton* dashboardSoundOffButton_ = nullptr;
    QPushButton* uiaRefreshButton_ = nullptr;
    QLabel* batchProgress_ = nullptr;
    QPushButton* cancelRemaining_ = nullptr;

    QPushButton* inspectButton_ = nullptr;
    QComboBox* chromeWindowCombo_ = nullptr;
    QTabWidget* diagnosticsTabs_ = nullptr;
    QPlainTextEdit* liveDiagnostics_ = nullptr;
    QPlainTextEdit* accessibilityTree_ = nullptr;

    QLabel* meetingCountValue_ = nullptr;
    QLabel* meetingCountDetail_ = nullptr;
    QLabel* uiaValue_ = nullptr;
    QLabel* uiaDetail_ = nullptr;

    QLabel* meetingCountLabel_ = nullptr;
    QLabel* meetingEmptyLabel_ = nullptr;
    QGridLayout* meetingsLayout_ = nullptr;
    QScrollArea* meetingsArea_ = nullptr;
    QWidget* meetingEmptyCard_ = nullptr;

    QList<ManagedMeeting> meetings_;
    QList<QFrame*> meetingCards_;
    QString dragSourceKey_;
    QPoint dragPressGlobal_;
    bool dragMoved_ = false;
    bool dragCursorSet_ = false;
    QFrame* dropHoverCard_ = nullptr;

    bool scanInProgress_ = false;
    bool pickupInProgress_ = false;
    bool meetingBusy_ = false;
};
