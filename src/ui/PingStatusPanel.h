#pragma once

#include <QWidget>

class QPlainTextEdit;
class QProcess;
class QTimer;

class PingStatusPanel final : public QWidget {
    Q_OBJECT
public:
    explicit PingStatusPanel(QWidget* parent);
    ~PingStatusPanel() override;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void startPing();
    void stopMonitoring();
    void readOutput(bool flushLastLine);
    void appendUnavailable(const QString& reason);
    void retry(const QString& reason);

    QPlainTextEdit* output_;
    QProcess* process_;
    QTimer* retryTimer_;
    bool monitoring_ = false;
    bool readingReplies_ = false;
};
