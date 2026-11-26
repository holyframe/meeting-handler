#include "PingStatusPanel.h"
#include "captions/Util.h"

#include <windows.h>
#include <QFontDatabase>
#include <QHideEvent>
#include <QPlainTextEdit>
#include <QProcess>
#include <QRegularExpression>
#include <QStyle>
#include <QTextOption>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QString consoleLine(QByteArray bytes) {
    while (bytes.endsWith('\n') || bytes.endsWith('\r')) bytes.chop(1);
    if (bytes.isEmpty()) return {};
    // Windows console utilities write localized text in the OEM code page.
    std::wstring wide(bytes.size(), L'\0');
    const int length = static_cast<int>(bytes.size());
    const int written = ::MultiByteToWideChar(CP_OEMCP, 0, bytes.constData(), length, wide.data(), length);
    return written ? QString::fromWCharArray(wide.data(), written) : QString::fromLocal8Bit(bytes);
}

QString replyMeasurements(const QString& line) {
    // Windows translates the field names but prints the values in a fixed order.
    // IPv6 replies can report only the round-trip time.
    static const QRegularExpression fields(QStringLiteral(
        R"([:：]\s*(?:[^:：=<>]+=\s*(?<bytes>\d+)\s+)?)"
        R"([^:：=<>]+(?<op>[=<])\s*(?<time>\d+)\s*\p{L}+)"
        R"((?:\s+[^:：=<>]+=\s*(?<ttl>\d+))?$)"));
    const auto match = fields.match(line);
    if (!match.hasMatch()) return {};
    const auto bytes = match.captured("bytes");
    const auto ttl = match.captured("ttl");
    return QStringLiteral("bytes=%1 time%2%3ms TTL=%4")
        .arg(bytes.isEmpty() ? QStringLiteral("—") : bytes, match.captured("op"),
             match.captured("time"), ttl.isEmpty() ? QStringLiteral("—") : ttl);
}
}

PingStatusPanel::PingStatusPanel(QWidget* parent)
    : QWidget(parent), output_(new QPlainTextEdit(this)), process_(new QProcess(this)),
      retryTimer_(new QTimer(this)) {
    setObjectName("pingStatusPanel");
    setAttribute(Qt::WA_StyledBackground);
    setAccessibleName(tr("Ping status for google.com"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setMaximumHeight(160);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 0, 4, 0);
    layout->addWidget(output_, 1);
    output_->setObjectName("pingOutput");
    output_->setAccessibleName(tr("Live ping output for google.com"));
    output_->setPlaceholderText(QStringLiteral("bytes=— time=— TTL=—"));
    output_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output_->setMinimumHeight(2 * output_->fontMetrics().lineSpacing() + 4);
    output_->setReadOnly(true);
    output_->setTabChangesFocus(true);
    output_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    output_->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    output_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    output_->setMaximumBlockCount(300);

    process_->setProgram(QString::fromStdWString(util::SystemExecutablePath(L"ping.exe")));
    process_->setArguments({QStringLiteral("google.com"), QStringLiteral("-t")});
    process_->setProcessChannelMode(QProcess::MergedChannels);
    process_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    retryTimer_->setSingleShot(true);
    retryTimer_->setInterval(5000);
    connect(retryTimer_, &QTimer::timeout, this, &PingStatusPanel::startPing);
    connect(process_, &QProcess::readyReadStandardOutput, this, [this] { readOutput(false); });
    connect(process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (!monitoring_) return;
        readOutput(true);
        retry(status == QProcess::CrashExit ? tr("Ping stopped unexpectedly.") : tr("Ping exited (code %1).").arg(code));
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            retry(tr("Could not start ping (%1).").arg(process_->errorString()));
    });
}

PingStatusPanel::~PingStatusPanel() {
    stopMonitoring();
}

void PingStatusPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (monitoring_) return;
    monitoring_ = true;
    setWarning(false);
    output_->clear();
    output_->setToolTip({});
    startPing();
}

void PingStatusPanel::hideEvent(QHideEvent* event) {
    if (!event->spontaneous()) stopMonitoring();
    QWidget::hideEvent(event);
}

void PingStatusPanel::startPing() {
    if (!monitoring_ || process_->state() != QProcess::NotRunning) return;
    readingReplies_ = false;
    process_->start(QIODevice::ReadOnly);
}

void PingStatusPanel::stopMonitoring() {
    monitoring_ = false;
    retryTimer_->stop();
    if (process_->state() != QProcess::NotRunning) {
        process_->kill();
        process_->waitForFinished(1500);
    }
}

void PingStatusPanel::readOutput(bool flushLastLine) {
    const auto appendLine = [this](const QByteArray& bytes) {
        const QString line = consoleLine(bytes).trimmed();
        if (line.isEmpty() || line.endsWith(u':') || line.endsWith(u'：')) {
            // The opening banner contains the resolved address in brackets.
            // A blank line separates the replies from the final statistics.
            readingReplies_ = line.contains(u'[') && line.contains(u']');
            return;
        }
        const QString measurements = replyMeasurements(line);
        if (!measurements.isEmpty()) {
            readingReplies_ = true;
            setWarning(false);
            output_->appendPlainText(measurements);
            output_->setToolTip({});
        } else if (readingReplies_) {
            appendUnavailable(tr("Unreachable"), line);
        }
    };
    while (process_->canReadLine()) appendLine(process_->readLine());
    if (flushLastLine) appendLine(process_->readAllStandardOutput());
}

void PingStatusPanel::setWarning(bool warning) {
    if (property("warning").toBool() == warning) return;
    setProperty("warning", warning);
    for (QWidget* widget : {static_cast<QWidget*>(this), static_cast<QWidget*>(output_)}) {
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
        widget->update();
    }
}

void PingStatusPanel::appendUnavailable(const QString& message, const QString& reason) {
    setWarning(true);
    output_->appendPlainText(message);
    output_->setToolTip(reason);
}

void PingStatusPanel::retry(const QString& reason) {
    if (!monitoring_ || retryTimer_->isActive()) return;
    appendUnavailable(tr("Ping unavailable"), tr("%1 Retrying in 5 seconds.").arg(reason));
    retryTimer_->start();
}
