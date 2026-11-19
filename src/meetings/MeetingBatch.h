#pragma once

#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

class QProcess;

// Runs dashboard operations one meeting at a time. A private worker process
// can be stopped even while Chrome is blocking inside a native UIA call.
class MeetingBatch final : public QObject
{
    Q_OBJECT

public:
    enum class Operation { Mute, SoundOff, Refresh };
    static constexpr int MeetingTimeoutMs = 12000;

    explicit MeetingBatch(QObject* parent = nullptr, int timeoutMs = MeetingTimeoutMs);
    ~MeetingBatch() override;

    void start(Operation operation, const QJsonArray& targets);
    void cancelRemaining();
    static int runWorker();

signals:
    void meetingStarted(int number, int total, const QString& key);
    void meetingFinished(const QJsonObject& result);
    void finished(int skipped);
    void diagnostic(const QString& message);
    void initialized(bool success, const QString& message);

private:
    void startNext();
    void startWorker();
    void sendCurrent();
    void readMessages(QProcess* source);
    void completeCurrent(QJsonObject result);
    void failCurrent(const QString& error, bool timedOut = false);
    void finishBatch();

    QProcess* worker_ = nullptr;
    QTimer timer_;
    QDeadlineTimer deadline_;
    QJsonArray targets_;
    Operation operation_ = Operation::Refresh;
    int index_ = 0;
    bool waiting_ = false;
    bool cancelRequested_ = false;
};
