#pragma once

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <windows.h>
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>

class UiaWorker final : public QObject
{
    Q_OBJECT

public:
    explicit UiaWorker(QObject* parent = nullptr);
    ~UiaWorker() override;

public slots:
    void initialize();
    void scanChromeWindows();
    void inspectWindow(quintptr hwndValue);
    void listChromeTabs();
    void activateMeeting(quintptr hwndValue,
                         const QString& tabRuntimeId,
                         const QString& tabName,
                         const QString& requestKey);
    void readMicrophone(quintptr hwndValue,
                        const QString& tabRuntimeId,
                        const QString& tabName,
                        const QString& requestKey);
    void setMicrophoneMuted(quintptr hwndValue,
                            const QString& tabRuntimeId,
                            const QString& tabName,
                            bool muted,
                            const QString& requestKey);
    void readAudioConnection(quintptr hwndValue,
                             const QString& tabRuntimeId,
                             const QString& tabName,
                             const QString& requestKey);
    void setAudioConnected(quintptr hwndValue,
                           const QString& tabRuntimeId,
                           const QString& tabName,
                           bool connected,
                           const QString& requestKey);
    void prepareImportedMeetings(const QJsonArray& targets);
    void shutdown();

signals:
    void initialized(bool success, const QString& message);
    void chromeWindowsFound(const QJsonArray& windows);
    void inspectionFinished(const QJsonObject& result);
    void chromeTabsListed(const QJsonObject& result);
    void meetingActivated(const QJsonObject& result);
    void microphoneRead(const QJsonObject& result);
    void microphoneSet(const QJsonObject& result);
    void audioConnectionRead(const QJsonObject& result);
    void audioConnectionSet(const QJsonObject& result);
    void importedMeetingsPrepared(const QJsonArray& results);
    void diagnostic(const QString& message);

private:
    bool comInitialized_ = false;
    Microsoft::WRL::ComPtr<IUIAutomation> automation_;
    Microsoft::WRL::ComPtr<IUIAutomation2> automation2_;
};
