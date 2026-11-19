#include "MeetingBatch.h"

#include "UiaWorker.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QProcess>

#include <iostream>
#include <string>
#include <utility>

namespace
{
const QString timeoutError = QStringLiteral("Verification timed out. The meeting's state is unread.");

void writeMessage(const QJsonObject& message)
{
    const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    std::cout.write(bytes.constData(), bytes.size());
    std::cout << '\n' << std::flush;
}
}

MeetingBatch::MeetingBatch(QObject* parent, int timeoutMs)
    : QObject(parent)
{
    timer_.setInterval(timeoutMs);
    timer_.setSingleShot(true);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, [this] { failCurrent(timeoutError, true); });
}

MeetingBatch::~MeetingBatch()
{
    // Includes an idle worker finishing on EOF or a timed-out worker exiting.
    for (QProcess* child : findChildren<QProcess*>(Qt::FindDirectChildrenOnly))
    {
        disconnect(child, nullptr, this, nullptr);
        if (child->state() != QProcess::NotRunning)
        {
            child->kill();
            child->waitForFinished();
        }
    }
}

void MeetingBatch::start(Operation operation, const QJsonArray& targets)
{
    if (!targets_.isEmpty() || targets.isEmpty())
        return;
    operation_ = operation;
    targets_ = targets;
    index_ = 0;
    cancelRequested_ = false;
    startNext();
}

void MeetingBatch::cancelRemaining()
{
    cancelRequested_ = true;
}

void MeetingBatch::startNext()
{
    if (cancelRequested_ || index_ >= targets_.size())
    {
        finishBatch();
        return;
    }

    waiting_ = true;
    deadline_ = QDeadlineTimer(timer_.interval(), Qt::PreciseTimer);
    timer_.start();
    emit meetingStarted(index_ + 1, static_cast<int>(targets_.size()),
                        targets_.at(index_).toObject().value("requestKey").toString());
    if (worker_)
        sendCurrent();
    else
        startWorker();
}

void MeetingBatch::startWorker()
{
    auto* process = new QProcess(this);
    worker_ = process;
    process->setStandardErrorFile(QProcess::nullDevice());
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    connect(process, &QProcess::started, this, &MeetingBatch::sendCurrent);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] { readMessages(process); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            failCurrent("The meeting worker could not start. " + process->errorString());
    });
    connect(process, &QProcess::finished, this, [this, process] {
        readMessages(process);
        if (worker_ != process)
            return;
        if (waiting_)
            failCurrent("The meeting worker exited before it could verify the result.");
        else
        {
            worker_ = nullptr;
            process->deleteLater();
        }
    });
    process->start(QCoreApplication::applicationFilePath(), {"--meeting-batch-worker"});
}

void MeetingBatch::sendCurrent()
{
    if (deadline_.hasExpired())
    {
        failCurrent(timeoutError, true);
        return;
    }
    QJsonObject request = targets_.at(index_).toObject();
    request.insert("operation", static_cast<int>(operation_));
    const QByteArray message = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (worker_->write(message) != message.size())
        failCurrent("The meeting worker could not receive the request.");
}

void MeetingBatch::readMessages(QProcess* source)
{
    while (worker_ == source && waiting_ && source->canReadLine())
    {
        if (deadline_.hasExpired())
        {
            failCurrent(timeoutError, true);
            return;
        }
        const QJsonObject message = QJsonDocument::fromJson(source->readLine()).object();
        const QString type = message.value("type").toString();
        if (type == "diagnostic")
            emit diagnostic(message.value("message").toString());
        else if (type == "initialized")
            emit initialized(message.value("ok").toBool(), message.value("message").toString());
        else
        {
            const QJsonObject result = message.value("result").toObject();
            if (type != "result" || !result.contains("ok")
                || result.value("requestKey") != targets_.at(index_).toObject().value("requestKey"))
            {
                failCurrent("The meeting worker returned an unexpected response.");
                return;
            }
            completeCurrent(result);
            QTimer::singleShot(0, this, &MeetingBatch::startNext);
            return;
        }
    }
}

void MeetingBatch::completeCurrent(QJsonObject result)
{
    timer_.stop();
    waiting_ = false;
    result.insert("requestKey", targets_.at(index_).toObject().value("requestKey"));
    ++index_;
    emit meetingFinished(result);
}

void MeetingBatch::failCurrent(const QString& error, bool timedOut)
{
    if (!waiting_)
        return;
    QProcess* failed = std::exchange(worker_, nullptr);
    if (failed)
        disconnect(failed, nullptr, this, nullptr);

    // Discard late output. A replacement may act only after the previous
    // process has exited, so two workers can never operate on Chrome together.
    if (failed && failed->state() != QProcess::NotRunning)
    {
        connect(failed, &QProcess::stateChanged, this, [this, failed](QProcess::ProcessState state) {
            if (state == QProcess::Running)
                failed->kill();
            else if (state == QProcess::NotRunning)
            {
                failed->deleteLater();
                QTimer::singleShot(0, this, &MeetingBatch::startNext);
            }
        });
        if (failed->state() == QProcess::Running)
            failed->kill();
    }
    else
    {
        if (failed)
            failed->deleteLater();
        QTimer::singleShot(0, this, &MeetingBatch::startNext);
    }
    completeCurrent({{"ok", false}, {"verified", false}, {"timedOut", timedOut}, {"error", error}});
}

void MeetingBatch::finishBatch()
{
    if (QProcess* idle = std::exchange(worker_, nullptr))
    {
        disconnect(idle, nullptr, this, nullptr);
        connect(idle, &QProcess::finished, idle, &QObject::deleteLater);
        idle->closeWriteChannel();
        if (idle->state() == QProcess::NotRunning)
            idle->deleteLater();
    }
    const int skipped = static_cast<int>(targets_.size()) - index_;
    targets_ = {};
    emit finished(skipped);
}

int MeetingBatch::runWorker()
{
    UiaWorker worker;
    const auto reply = [](const QJsonObject& result) {
        writeMessage({{"type", "result"}, {"result", result}});
    };
    QObject::connect(&worker, &UiaWorker::microphoneSet, reply);
    QObject::connect(&worker, &UiaWorker::audioConnectionSet, reply);
    QObject::connect(&worker, &UiaWorker::audioConnectionRead, reply);
    QObject::connect(&worker, &UiaWorker::diagnostic, [](const QString& message) {
        writeMessage({{"type", "diagnostic"}, {"message", message}});
    });
    QObject::connect(&worker, &UiaWorker::initialized, [](bool ok, const QString& message) {
        writeMessage({{"type", "initialized"}, {"ok", ok}, {"message", message}});
    });

    std::string line;
    while (std::getline(std::cin, line))
    {
        const QJsonObject request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const QString key = request.value("requestKey").toString();
        const QString id = request.value("tabRuntimeId").toString();
        const QString name = request.value("tabName").toString();
        bool validHandle = false;
        const auto hwnd = static_cast<quintptr>(request.value("windowHandle").toString().toULongLong(&validHandle, 16));
        if (!validHandle || !hwnd || key.isEmpty())
        {
            reply({{"ok", false}, {"requestKey", key}, {"error", "The Chrome window handle or meeting key is invalid."}});
            continue;
        }
        switch (static_cast<Operation>(request.value("operation").toInt(-1)))
        {
        case Operation::Mute: worker.setMicrophoneMuted(hwnd, id, name, true, key); break;
        case Operation::SoundOff: worker.setAudioConnected(hwnd, id, name, false, key); break;
        case Operation::Refresh: worker.readAudioConnection(hwnd, id, name, key); break;
        default: reply({{"ok", false}, {"requestKey", key}, {"error", "Unknown meeting batch operation."}}); break;
        }
    }
    return 0;
}
