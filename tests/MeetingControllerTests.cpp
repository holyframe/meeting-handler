#include "ui/MeetingControllerPane.h"
#include "windows/UiaWorker.h"
#include "meetings/ZoomAudioState.h"

#include <QApplication>
#include <QFrame>
#include <QFontDatabase>
#include <QGridLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <cstring>
#include <iostream>
#include <string>

using zoomAudio::Action;
using zoomAudio::Connection;

namespace
{
// The test executable acts as its own worker so the real scheduler, pipes,
// and process deadlines can be exercised without opening a Chrome tab.
int runTestBatchWorker()
{
    std::string line;
    while (std::getline(std::cin, line))
    {
        const auto request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const QString id = request.value("tabRuntimeId").toString();
        if (id == "exit")
            return 7;
        if (id == "stall")
        {
            std::getline(std::cin, line);
            return 0;
        }
        const auto operation = static_cast<MeetingBatch::Operation>(request.value("operation").toInt());
        const bool off = operation == MeetingBatch::Operation::SoundOff;
        QJsonObject result{
            {"requestKey", id == "wrong-key" ? QJsonValue("different-meeting") : request.value("requestKey")},
            {"ok", true}, {"verified", true}, {"changed", !id.startsWith("already")},
            {"state", off ? "audioNotConnected" : "muted"}, {"fixturePid", QCoreApplication::applicationPid()}
        };
        if (operation != MeetingBatch::Operation::Mute)
            result.insert("audioState", off ? "disconnected" : "connected");
        if (id == "new-id")
            result.insert("tabRuntimeId", "resolved-tab");
        const QJsonObject message{{"type", "result"}, {"result", result}};
        std::cout << QJsonDocument(message).toJson(QJsonDocument::Compact).constData() << '\n' << std::flush;
    }
    return 0;
}

struct FakeAudio
{
    QList<QStringList> frames;
    QList<Action> actions;
    int frame = 0;
    bool canInvoke = true;
    bool chooserRequiresToolbar = false;

    zoomAudio::Backend backend()
    {
        return {
            [this] { return zoomAudio::observe(frames.at(qMin(frame, frames.size() - 1))); },
            [this](Action action) { actions.append(action); return canInvoke; },
            [this] {
                if (frame > 0 || !chooserRequiresToolbar || actions.contains(Action::JoinAudio))
                    ++frame;
            }
        };
    }
};
}

class MeetingControllerTests : public QObject
{
    Q_OBJECT

    static void populate(MeetingControllerPane& window, ManagedMeeting::Sound sound,
                         ManagedMeeting::Microphone microphone)
    {
        window.resize(1120, 720);
        // UI tests exercise card signals without calling UIA or touching Chrome.
        QObject::disconnect(&window, nullptr, window.worker_, nullptr);
        ManagedMeeting meeting;
        meeting.windowHandle = "1234";
        meeting.tabRuntimeId = "test-tab";
        meeting.title = "Audio test meeting";
        meeting.profileName = "Test profile";
        meeting.meetingId = "123456789";
        meeting.sound = sound;
        meeting.microphone = microphone;
        window.meetings_.append(meeting);
        window.refreshMeetingCards();
    }

private slots:
    void initTestCase()
    {
        // The offscreen plugin has no Windows font discovery. Load the same
        // installed family the real Windows dashboard uses for visual checks.
        const QString fonts = qEnvironmentVariable("WINDIR") + "/Fonts/";
        QVERIFY(QFontDatabase::addApplicationFont(fonts + "segoeui.ttf") >= 0);
        QVERIFY(QFontDatabase::addApplicationFont(fonts + "segoeuib.ttf") >= 0);
        QVERIFY(QFontDatabase::addApplicationFont(fonts + "seguisb.ttf") >= 0);
    }

    void recognizesAudioControls_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("action");
        QTest::addColumn<int>("connection");
        const auto row = [](const char* label, const QString& name, Action action, Connection connection)
        {
            QTest::newRow(label) << name << static_cast<int>(action) << static_cast<int>(connection);
        };
        row("muted mic", "Unmute", Action::None, Connection::Unknown);
        row("live mic", "Mute", Action::None, Connection::Unknown);
        row("shortcut", "Unmute (Alt+A)", Action::None, Connection::Unknown);
        row("my mic", "Mute my microphone", Action::None, Connection::Unknown);
        row("join", "Join Audio", Action::JoinAudio, Connection::Disconnected);
        row("computer join", "Join with Computer Audio", Action::JoinComputerAudio, Connection::Disconnected);
        row("alternate join", "Join Audio by Computer", Action::JoinComputerAudio, Connection::Disconnected);
        row("join button with loading text", "Join Audio by Computer loading", Action::JoinComputerAudio, Connection::Disconnected);
        row("connect", "Connect to Computer Audio", Action::JoinComputerAudio, Connection::Disconnected);
        row("leave", "Leave Computer Audio", Action::LeaveAudio, Connection::Connected);
        row("disconnect", "Disconnect Audio", Action::LeaveAudio, Connection::Connected);
        row("menu", "Audio options", Action::OpenMenu, Connection::Unknown);
        row("menu with mute", "More mute controls", Action::OpenMenu, Connection::Unknown);
        row("live chooser button", "Join Audio by Computer", Action::JoinComputerAudio, Connection::Disconnected);
        row("live toolbar join", "join audio", Action::JoinAudio, Connection::Disconnected);
        row("computer audio tab", "Computer Audio", Action::SelectComputerAudio, Connection::Unknown);
        row("participants", "Mute All", Action::None, Connection::Unknown);
        row("unmute participants", "Unmute All", Action::None, Connection::Unknown);
        row("another participant", "Mute Alice", Action::None, Connection::Unknown);
        row("auto join setting", "Automatically join computer audio", Action::None, Connection::Unknown);
        row("test speaker", "Test Speaker", Action::None, Connection::Unknown);
        row("speaker device", "Speakers (Realtek Audio)", Action::None, Connection::Unknown);
    }

    void recognizesAudioControls()
    {
        QFETCH(QString, name);
        QFETCH(int, action);
        QFETCH(int, connection);
        QCOMPARE(static_cast<int>(zoomAudio::actionForName(name)), action);
        QCOMPARE(static_cast<int>(zoomAudio::observe({name}).connection), connection);
    }

    void refusesContradictoryState()
    {
        FakeAudio fake{{{"Mute", "Join Audio"}}};
        const auto result = zoomAudio::setConnected(fake.backend(), false, 3);
        QVERIFY(!result.ok);
        QVERIFY(!result.actionAttempted);
        QVERIFY(fake.actions.isEmpty());
        QCOMPARE(result.observation.microphoneState, "unknown");
    }

    void alreadyConnectedDoesNotUnmuteMicrophone()
    {
        FakeAudio fake{{{"Unmute", "Leave Computer Audio"}}};
        const auto result = zoomAudio::setConnected(fake.backend(), true, 3);
        QVERIFY(result.ok);
        QVERIFY(!result.changed);
        QVERIFY(!result.actionAttempted);
        QCOMPARE(result.observation.microphoneState, "muted");
        QVERIFY(fake.actions.isEmpty());
    }

    void alreadyDisconnectedDoesNotJoin()
    {
        FakeAudio fake{{{"Join Audio"}}};
        const auto result = zoomAudio::setConnected(fake.backend(), false, 3);
        QVERIFY(result.ok);
        QVERIFY(!result.changed);
        QVERIFY(fake.actions.isEmpty());
    }

    void leavesThroughAudioMenuAndVerifies()
    {
        FakeAudio fake{{{"Unmute", "Audio options"},
                        {"Unmute", "Audio options", "Leave Computer Audio"},
                        {"Join Audio"}}};
        const auto result = zoomAudio::setConnected(fake.backend(), false, 5);
        QVERIFY(result.ok);
        QVERIFY(result.changed);
        QCOMPARE(fake.actions, QList<Action>({Action::OpenMenu, Action::LeaveAudio}));
        QCOMPARE(result.observation.connection, Connection::Disconnected);
        QCOMPARE(result.observation.microphoneState, "audioNotConnected");
    }

    void joinsThroughChooserAndVerifies()
    {
        FakeAudio fake{{{"Join Audio"}, {"Join Audio", "Join with Computer Audio"}, {},
                        {"Unmute", "Leave Computer Audio"}}};
        fake.chooserRequiresToolbar = true;
        const auto result = zoomAudio::setConnected(fake.backend(), true, 6);
        QVERIFY(result.ok);
        QVERIFY(result.changed);
        QCOMPARE(fake.actions, QList<Action>({Action::JoinAudio, Action::JoinComputerAudio}));
        QCOMPARE(result.observation.connection, Connection::Connected);
        QCOMPARE(result.observation.microphoneState, "muted");
    }

    void joinsDirectlyWhenChooserAlreadyOpen()
    {
        FakeAudio fake{{{"Join with Computer Audio"}, {"Mute", "Leave Computer Audio"}}};
        const auto result = zoomAudio::setConnected(fake.backend(), true, 3);
        QVERIFY(result.ok);
        QCOMPARE(fake.actions, QList<Action>({Action::JoinComputerAudio}));
        QCOMPARE(result.observation.microphoneState, "unmuted");
    }

    void explicitComputerJoinOutweighsStaleToolbarMicrophone()
    {
        const auto observation = zoomAudio::observe(
            {"unmute my microphone", "join audio", "Computer Audio", "Join Audio by Computer"});
        QCOMPARE(observation.connection, Connection::Disconnected);
        QCOMPARE(observation.microphoneState, "audioNotConnected");
        QVERIFY(observation.joinComputerAvailable);
    }

    void selectsComputerPanelBeforeJoining()
    {
        FakeAudio fake{{{"join audio"},
                        {"join audio", "Phone Call", "Computer Audio"},
                        {"join audio", "Computer Audio", "Join Audio by Computer"},
                        {"unmute my microphone", "More audio controls"},
                        {"unmute my microphone", "More audio controls", "Leave Computer Audio"}}};
        fake.chooserRequiresToolbar = true;
        const auto result = zoomAudio::setConnected(fake.backend(), true, 10);
        QVERIFY(result.ok);
        QVERIFY(result.changed);
        QCOMPARE(result.observation.microphoneState, "muted");
        QCOMPARE(fake.actions, QList<Action>({Action::JoinAudio, Action::SelectComputerAudio,
                                              Action::JoinComputerAudio, Action::OpenMenu}));
    }

    void existingChooserNeverClicksToolbarJoinAgain()
    {
        FakeAudio fake{{{"join audio", "Phone Call", "Computer Audio"},
                        {"join audio", "Computer Audio", "Join Audio by Computer"},
                        {"unmute my microphone", "Leave Computer Audio"}}};
        const auto result = zoomAudio::setConnected(fake.backend(), true, 10);
        QVERIFY(result.ok);
        QCOMPARE(fake.actions, QList<Action>({Action::SelectComputerAudio, Action::JoinComputerAudio}));
    }

    void soundOffOpensChooserBeforeSoundOn()
    {
        FakeAudio fake{{{"unmute my microphone", "More audio controls"},
                        {"unmute my microphone", "More audio controls", "Leave computer audio"},
                        {"join audio"},
                        {"join audio", "Phone Call", "Computer Audio", "Join Audio by Computer"},
                        {"unmute my microphone", "More audio controls"},
                        {"unmute my microphone", "More audio controls", "Leave computer audio"}}};
        const auto backend = fake.backend();
        const auto off = zoomAudio::setConnected(backend, false, 8);
        QVERIFY(off.ok);
        QCOMPARE(off.observation.connection, Connection::Disconnected);
        const auto on = zoomAudio::setConnected(backend, true, 8);
        QVERIFY(on.ok);
        QVERIFY(on.changed);
        QCOMPARE(on.observation.microphoneState, "muted");
        QCOMPARE(fake.actions, QList<Action>({Action::OpenMenu, Action::LeaveAudio,
                                              Action::JoinComputerAudio, Action::OpenMenu}));
    }

    void waitsForJoinButtonInsideVisibleComputerPanel()
    {
        FakeAudio fake{{{"Join Audio"}, {"Join Audio", "Computer Audio"}, {"Join Audio"}, {"Join Audio"},
                        {"Join Audio", "Join Audio by Computer"}, {"Unmute", "Leave Computer Audio"}}};
        auto backend = fake.backend();
        backend.read = [&] {
            auto state = zoomAudio::observe(fake.frames.at(fake.frame));
            state.computerAudioPanelVisible = fake.frame < 2;
            return state;
        };
        const auto on = zoomAudio::setConnected(backend, true, 8);
        QVERIFY(on.ok);
        QCOMPARE(fake.actions, QList<Action>({Action::JoinComputerAudio}));
    }

    void soundOnAfterManualRejoinOnlyRefreshesCardState()
    {
        FakeAudio fake{{{"Computer Audio", "unmute my microphone", "More audio controls"},
                        {"unmute my microphone", "More audio controls", "Leave computer audio"}}};
        const auto on = zoomAudio::setConnected(fake.backend(), true, 8);
        QVERIFY(on.ok);
        QVERIFY(!on.changed);
        QVERIFY(!on.actionAttempted);
        QCOMPARE(on.observation.connection, Connection::Connected);
        QCOMPARE(on.observation.microphoneState, "muted");
        QCOMPARE(fake.actions, QList<Action>({Action::OpenMenu}));
    }

    void manualRejoinDuringChooserDiscoveryIsVerified()
    {
        FakeAudio fake{{{"Join Audio"}, {"unmute my microphone", "More audio controls"},
                        {"unmute my microphone", "More audio controls", "Leave computer audio"}}};
        const auto on = zoomAudio::setConnected(fake.backend(), true, 8);
        QVERIFY(on.ok);
        QVERIFY(!on.actionAttempted);
        QVERIFY(!on.changed);
        QCOMPARE(fake.actions, QList<Action>({Action::OpenMenu}));
    }

    void verifiesWhenZoomRemovesJoinButtonBeforeActivation()
    {
        FakeAudio fake{{{"Join Audio by Computer"}, {"Unmute", "More audio controls"},
                        {"Unmute", "More audio controls", "Leave computer audio"}}};
        auto backend = fake.backend();
        backend.invoke = [&](Action action) {
            fake.actions.append(action);
            if (action == Action::JoinComputerAudio)
            {
                ++fake.frame; // The user joined between read and activation.
                return false;
            }
            return action == Action::OpenMenu;
        };
        const auto on = zoomAudio::setConnected(backend, true, 8);
        QVERIFY(on.ok);
        QVERIFY(!on.changed);
        QCOMPARE(on.observation.connection, Connection::Connected);
        QCOMPARE(fake.actions, QList<Action>({Action::JoinComputerAudio, Action::OpenMenu}));
    }

    void readsChooserPanelWithoutJoiningAudio()
    {
        FakeAudio fake{{{"Phone Call", "Computer Audio"},
                        {"Computer Audio", "Join Audio by Computer"}}};
        const auto result = zoomAudio::readConnection(fake.backend(), 5);
        QVERIFY(result.ok);
        QVERIFY(!result.actionAttempted);
        QCOMPARE(result.observation.connection, Connection::Disconnected);
        QCOMPARE(fake.actions, QList<Action>({Action::SelectComputerAudio}));
    }

    void delayedJoinButtonIsInvokedOnce()
    {
        FakeAudio fake{{{"join audio"}}};
        fake.chooserRequiresToolbar = true;
        for (int index = 0; index < 18; ++index)
            fake.frames.append(QStringList{"join audio", "Computer Audio"});
        fake.frames.append(QStringList{"join audio", "Computer Audio", "Join Audio by Computer"});
        fake.frames.append(QStringList{"unmute my microphone", "Leave Computer Audio"});
        const auto result = zoomAudio::setConnected(fake.backend(), true, 50);
        QVERIFY(result.ok);
        QCOMPARE(fake.actions, QList<Action>({Action::JoinAudio, Action::SelectComputerAudio,
                                              Action::JoinComputerAudio}));
    }

    void failedJoinDoesNotKeepClicking()
    {
        FakeAudio fake{{{"Join Audio"}, {"Join Audio", "Join with Computer Audio"}}};
        fake.chooserRequiresToolbar = true;
        const auto result = zoomAudio::setConnected(fake.backend(), true, 5);
        QVERIFY(!result.ok);
        QVERIFY(result.actionAttempted);
        QVERIFY(!result.changed);
        QCOMPARE(fake.actions, QList<Action>({Action::JoinAudio, Action::JoinComputerAudio}));
        QVERIFY(result.error.contains("unverified"));
    }

    void joiningMustVerifyComputerAudioThroughMenu()
    {
        FakeAudio fake{{{"Join Audio"}, {"Unmute", "Audio options"},
                        {"Unmute", "Audio options", "Leave Computer Audio"}}};
        fake.chooserRequiresToolbar = true;
        const auto result = zoomAudio::setConnected(fake.backend(), true, 5);
        QVERIFY(result.ok);
        QCOMPARE(fake.actions, QList<Action>({Action::JoinAudio, Action::OpenMenu}));
        QCOMPARE(result.observation.controlName, "Leave Computer Audio");
    }

    void microphoneAloneCannotVerifySoundOn()
    {
        FakeAudio fake{{{"Join Audio"}, {"Unmute"}}};
        fake.chooserRequiresToolbar = true;
        const auto result = zoomAudio::setConnected(fake.backend(), true, 3);
        QVERIFY(!result.ok);
        QVERIFY(!result.changed);
        QCOMPARE(fake.actions, QList<Action>({Action::JoinAudio}));
    }

    void conflictingMicrophonesRemainUnread()
    {
        const auto observation = zoomAudio::observe({"Mute", "Unmute", "Leave Computer Audio"});
        QCOMPARE(observation.connection, Connection::Connected);
        QCOMPARE(observation.microphoneState, "unknown");
    }

    void failedLeaveDoesNotKeepClicking()
    {
        FakeAudio fake{{{"Mute", "Leave Computer Audio"}}};
        const auto result = zoomAudio::setConnected(fake.backend(), false, 5);
        QVERIFY(!result.ok);
        QVERIFY(!result.changed);
        QCOMPARE(fake.actions, QList<Action>({Action::LeaveAudio}));
    }

    void unknownStateNeverJoinsOrLeaves()
    {
        FakeAudio fake{{{"Audio options"}}};
        const auto result = zoomAudio::setConnected(fake.backend(), true, 3);
        QVERIFY(!result.ok);
        QVERIFY(!result.actionAttempted);
        QCOMPARE(fake.actions, QList<Action>({Action::OpenMenu}));
    }

    void missingLeaveControlDoesNotInvokeMicrophone()
    {
        FakeAudio fake{{{"Mute", "Audio options"}}};
        const auto result = zoomAudio::setConnected(fake.backend(), false, 3);
        QVERIFY(!result.ok);
        QVERIFY(!result.actionAttempted);
        QCOMPARE(fake.actions, QList<Action>({Action::OpenMenu}));
    }

    void unavailableActionIsReportedUnverified()
    {
        FakeAudio fake{{{"Join Audio"}}};
        fake.canInvoke = false;
        const auto result = zoomAudio::setConnected(fake.backend(), true, 3);
        QVERIFY(!result.ok);
        QVERIFY(result.actionAttempted);
        QVERIFY(result.error.contains("unverified"));
    }

    void readCanDiscoverConnectionThroughMenu()
    {
        FakeAudio fake{{{"Audio options"}, {"Leave Computer Audio"}}};
        const auto result = zoomAudio::readConnection(fake.backend(), 3);
        QVERIFY(result.ok);
        QVERIFY(!result.actionAttempted);
        QCOMPARE(result.observation.connection, Connection::Connected);
        QCOMPARE(fake.actions, QList<Action>({Action::OpenMenu}));
    }

    void soundOffButtonTargetsOnlyItsCardAndDisablesWhileBusy()
    {
        MeetingControllerPane window;
        populate(window, ManagedMeeting::Sound::On, ManagedMeeting::Microphone::Muted);
        QSignalSpy spy(&window, &MeetingControllerPane::audioSetRequested);
        auto* button = window.meetingCards_.first()->findChild<QPushButton*>("soundButton");
        QVERIFY(button);
        QCOMPARE(button->text(), "Sound Off");
        QVERIFY(button->toolTip().contains("microphone"));
        button->click();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toULongLong(), 0x1234ULL);
        QCOMPARE(spy.at(0).at(1).toString(), "test-tab");
        QCOMPARE(spy.at(0).at(3).toBool(), false);
        QCOMPARE(spy.at(0).at(4).toString(), window.meetings_.first().key());
        QVERIFY(window.meetingBusy_);
        QVERIFY(!window.meetingCards_.first()->findChild<QPushButton*>("soundButton")->isEnabled());
        window.setSound(window.meetings_.first().key(), false);
        QCOMPARE(spy.count(), 1);
    }

    void soundOnButtonReconnectsOnlyItsCard()
    {
        MeetingControllerPane window;
        populate(window, ManagedMeeting::Sound::Off, ManagedMeeting::Microphone::AudioNotConnected);
        QSignalSpy spy(&window, &MeetingControllerPane::audioSetRequested);
        auto* button = window.meetingCards_.first()->findChild<QPushButton*>("soundButton");
        QCOMPARE(button->text(), "Sound On");
        button->click();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(3).toBool(), true);
    }

    void secondCardDoesNotTargetFirstMeeting()
    {
        MeetingControllerPane window;
        populate(window, ManagedMeeting::Sound::On, ManagedMeeting::Microphone::Muted);
        auto second = window.meetings_.first();
        second.windowHandle = "5678";
        second.tabRuntimeId = "second-tab";
        second.title = "Second meeting";
        second.sound = ManagedMeeting::Sound::Off;
        second.microphone = ManagedMeeting::Microphone::AudioNotConnected;
        window.meetings_.append(second);
        window.refreshMeetingCards();
        QSignalSpy spy(&window, &MeetingControllerPane::audioSetRequested);
        window.meetingCards_.at(1)->findChild<QPushButton*>("soundButton")->click();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toULongLong(), 0x5678ULL);
        QCOMPARE(spy.at(0).at(1).toString(), "second-tab");
        QCOMPARE(spy.at(0).at(3).toBool(), true);
        QCOMPARE(window.meetings_.first().sound, ManagedMeeting::Sound::On);
        QCOMPARE(window.meetings_.first().microphone, ManagedMeeting::Microphone::Muted);
    }

    void missingSoundReadDoesNotDiscardReadableMicrophone()
    {
        ManagedMeeting meeting;
        meeting.applyObservation( {
            {"ok", true}, {"state", "muted"}, {"audioState", "unknown"},
            {"audioError", "Audio menu unavailable"}
        });
        QCOMPARE(meeting.microphone, ManagedMeeting::Microphone::Muted);
        QCOMPARE(meeting.sound, ManagedMeeting::Sound::Unknown);
    }

    void unknownSoundReadsBeforeOfferingChange()
    {
        MeetingControllerPane window;
        populate(window, ManagedMeeting::Sound::Unknown, ManagedMeeting::Microphone::Unknown);
        QSignalSpy readSpy(&window, &MeetingControllerPane::audioReadRequested);
        QSignalSpy setSpy(&window, &MeetingControllerPane::audioSetRequested);
        auto* button = window.meetingCards_.first()->findChild<QPushButton*>("soundButton");
        QCOMPARE(button->text(), "Read Sound");
        button->click();
        QCOMPARE(readSpy.count(), 1);
        QCOMPARE(setSpy.count(), 0);
    }

    void verifiedSoundResultRefreshesBothStatesAndIdentity()
    {
        MeetingControllerPane window;
        populate(window, ManagedMeeting::Sound::On, ManagedMeeting::Microphone::Muted);
        const QString oldKey = window.meetings_.first().key();
        window.onAudioConnectionSet({
            {"ok", true}, {"verified", true}, {"requestKey", oldKey},
            {"requestedConnected", false}, {"audioState", "disconnected"},
            {"audioControlName", "Join Audio"}, {"state", "audioNotConnected"},
            {"tabRuntimeId", "replacement-tab"}
        });
        QCOMPARE(window.meetings_.first().sound, ManagedMeeting::Sound::Off);
        QCOMPARE(window.meetings_.first().microphone, ManagedMeeting::Microphone::AudioNotConnected);
        QCOMPARE(window.meetings_.first().key(), QString("1234/replacement-tab"));
        QCOMPARE(window.meetingCards_.first()->objectName(), QString("meetingCardActive"));
        QCOMPARE(window.meetingCountValue_->text(), QString("1/1"));
        QCOMPARE(window.meetingCards_.first()->findChild<QPushButton*>("soundButton")->text(), "Sound On");
        QCOMPARE(window.meetingCards_.first()->findChild<QLabel*>("soundState")->text(), "Sound off");
    }

    void unverifiedSoundResultClearsCachedStates()
    {
        MeetingControllerPane window;
        populate(window, ManagedMeeting::Sound::On, ManagedMeeting::Microphone::Muted);
        QTimer::singleShot(0, []
        {
            if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                message->accept();
        });
        // Even an apparent success cannot update a card without verification.
        window.onAudioConnectionSet({
            {"ok", true}, {"verified", false}, {"requestedConnected", false},
            {"requestKey", window.meetings_.first().key()},
            {"audioState", "disconnected"}, {"state", "audioNotConnected"}
        });
        QCOMPARE(window.meetings_.first().sound, ManagedMeeting::Sound::Unknown);
        QCOMPARE(window.meetings_.first().microphone, ManagedMeeting::Microphone::Unknown);
        QCOMPARE(window.meetingCards_.first()->findChild<QPushButton*>("soundButton")->text(), "Read Sound");
        QVERIFY(!window.meetingBusy_);
    }

    void renamesCardWithoutChangingChromeTabName()
    {
        MeetingControllerPane window;
        populate(window, ManagedMeeting::Sound::On, ManagedMeeting::Microphone::Muted);
        window.meetings_.first().chromeTabName = "Audio test meeting";
        auto* edit = window.meetingCards_.first()->findChild<QLineEdit*>("meetingTitle");
        QVERIFY(edit);
        edit->setText("  Standup  ");
        emit edit->editingFinished();
        QCOMPARE(window.meetings_.first().title, QString("Standup"));
        QCOMPARE(window.meetings_.first().chromeTabName, QString("Audio test meeting"));
        QCOMPARE(window.meetings_.first().automationName(), QString("Audio test meeting"));
    }

    void reordersMeetingsByDragTarget()
    {
        MeetingControllerPane window;
        populate(window, ManagedMeeting::Sound::On, ManagedMeeting::Microphone::Muted);
        auto second = window.meetings_.first();
        second.tabRuntimeId = "second-tab";
        second.title = "Second meeting";
        second.chromeTabName = "Second meeting";
        window.meetings_.append(second);
        window.refreshMeetingCards();
        window.moveMeeting(second.key(), window.meetings_.first().key());
        QCOMPARE(window.meetings_.first().title, QString("Second meeting"));
        QCOMPARE(window.meetings_.last().title, QString("Audio test meeting"));
    }

    void refreshChecksEveryMeetingConnection()
    {
        MeetingControllerPane window;
        populate(window, ManagedMeeting::Sound::Unknown, ManagedMeeting::Microphone::Unknown);
        auto second = window.meetings_.first();
        window.meetings_.first().tabRuntimeId = "new-id";
        second.tabRuntimeId = "second";
        second.title = "Second meeting";
        window.meetings_.append(second);
        QSignalSpy started(window.batch_, &MeetingBatch::meetingStarted);
        QSignalSpy results(window.batch_, &MeetingBatch::meetingFinished);
        QSignalSpy done(window.batch_, &MeetingBatch::finished);
        QSignalSpy pickup(&window, &MeetingControllerPane::pickupRequested);
        QFrame* secondCard = nullptr;
        connect(window.batch_, &MeetingBatch::meetingFinished, this, [&](const QJsonObject& result) {
            if (result.value("requestKey").toString() != "1234/new-id")
                return;
            QVERIFY(window.meetingBusy_);
            QCOMPARE(window.meetingCountValue_->text(), "1/2");
            QCOMPARE(window.meetingCards_.first()->property("meetingKey").toString(), "1234/resolved-tab");
            QCOMPARE(window.meetingCards_.first()->findChild<QLabel*>("soundState")->text(), "Sound on");
            QCOMPARE(window.meetingCards_.at(1), secondCard);
            QCOMPARE(window.meetings_.at(1).sound, ManagedMeeting::Sound::Unknown);
        });
        window.uiaRefreshButton_->click();
        secondCard = window.meetingCards_.at(1);
        QCOMPARE(window.batchProgress_->text(), "Checking meeting 1 of 2");
        QVERIFY(!window.cancelRemaining_->isHidden());
        window.requestRefreshConnections();
        window.requestMeetingTabImport();
        QCOMPARE(pickup.size(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        QCOMPARE(started.size(), 2);
        QCOMPARE(started.at(1).at(2).toString(), second.key());
        QCOMPARE(results.size(), 2);
        QCOMPARE(results.first().first().toJsonObject().value("fixturePid"),
                 results.last().first().toJsonObject().value("fixturePid"));
        QCOMPARE(window.meetingCountValue_->text(), "2/2");
        QVERIFY(window.batchProgress_->isHidden());
        QVERIFY(window.uiaRefreshButton_->isEnabled());
        QVERIFY(window.statusMessage().contains("2 verified"));
    }

    void cancelRemainingFinishesCurrentAndSkipsTheRest_data()
    {
        QTest::addColumn<bool>("mute");
        QTest::newRow("Mute All") << true;
        QTest::newRow("Sound Off All") << false;
    }

    void cancelRemainingFinishesCurrentAndSkipsTheRest()
    {
        QFETCH(bool, mute);
        MeetingControllerPane window;
        populate(window, ManagedMeeting::Sound::On, ManagedMeeting::Microphone::Unmuted);
        auto next = window.meetings_.first();
        next.tabRuntimeId = "already";
        next.microphone = ManagedMeeting::Microphone::Muted;
        if (!mute)
            next.sound = ManagedMeeting::Sound::Off;
        window.meetings_.append(next);
        auto skipped = window.meetings_.first();
        skipped.tabRuntimeId = "skipped";
        window.meetings_.append(skipped);
        window.refreshMeetingCards();

        QSignalSpy started(window.batch_, &MeetingBatch::meetingStarted);
        QSignalSpy results(window.batch_, &MeetingBatch::meetingFinished);
        QSignalSpy done(window.batch_, &MeetingBatch::finished);
        const auto cancelClick = connect(window.batch_, &MeetingBatch::meetingStarted, this, [&](int number, int total, const QString&) {
            if (number != 2)
                return;
            QCOMPARE(total, 3);
            QCOMPARE(window.batchProgress_->text(), "Checking meeting 2 of 3");
            window.cancelRemaining_->click();
            QVERIFY(!window.cancelRemaining_->isEnabled());
            QVERIFY(window.meetingBusy_);
        });
        // Dismiss an unexpected prompt so the one-click assertion can report the regression.
        QTimer::singleShot(0, &window, [] {
            if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        (mute ? window.dashboardMuteAllButton_ : window.dashboardSoundOffButton_)->click();
        QCOMPARE(started.size(), 1);
        QVERIFY(!QApplication::activeModalWidget());
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        QCOMPARE(done.first().first().toInt(), 1);
        QCOMPARE(started.size(), 2);
        QCOMPARE(results.size(), 2);
        QCOMPARE(results.last().first().toJsonObject().value("requestKey").toString(), next.key());
        QCOMPARE(window.meetings_.first().microphone, mute ? ManagedMeeting::Microphone::Muted
                                                           : ManagedMeeting::Microphone::AudioNotConnected);
        QCOMPARE(window.meetings_.first().sound, mute ? ManagedMeeting::Sound::On : ManagedMeeting::Sound::Off);
        QCOMPARE(window.meetings_.last().sound, skipped.sound);
        QCOMPARE(window.meetings_.last().microphone, skipped.microphone);
        QVERIFY(window.statusMessage().contains(mute ? "1 muted, 1 already muted"
                                                                  : "1 turned off, 1 already off"));
        QVERIFY(window.statusMessage().contains("0 failed, 1 skipped"));
        QVERIFY(!window.meetingBusy_);
        QVERIFY(window.cancelRemaining_->isHidden());
        QVERIFY(window.dashboardMuteAllButton_->isEnabled());

        disconnect(cancelClick);
        window.uiaRefreshButton_->click();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 5000);
        QCOMPARE(done.last().first().toInt(), 0);
        QCOMPARE(results.size(), 5);
        QCOMPARE(results.last().first().toJsonObject().value("requestKey").toString(), skipped.key());
    }

    void batchRecoversFromWorkerFailure_data()
    {
        QTest::addColumn<QString>("fault");
        QTest::newRow("native call stalls") << QString("stall");
        QTest::newRow("worker exits") << QString("exit");
        QTest::newRow("mismatched meeting key") << QString("wrong-key");
    }

    void batchRecoversFromWorkerFailure()
    {
        QFETCH(QString, fault);
        MeetingControllerPane window;
        populate(window, ManagedMeeting::Sound::On, ManagedMeeting::Microphone::Muted);
        window.meetings_.first().tabRuntimeId = fault;
        window.refreshMeetingCards();
        MeetingBatch batch(nullptr, 1200);
        const QString failedKey = window.meetings_.first().key();
        connect(&batch, &MeetingBatch::meetingFinished, &window, [&](const QJsonObject& result) {
            if (result.value("requestKey").toString() == failedKey)
                window.onBatchMeetingFinished(result);
        });
        QSignalSpy started(&batch, &MeetingBatch::meetingStarted);
        QSignalSpy results(&batch, &MeetingBatch::meetingFinished);
        QSignalSpy done(&batch, &MeetingBatch::finished);
        QJsonArray targets;
        for (const QString& id : QStringList{"first", fault, "last"})
            targets.append(QJsonObject{{"requestKey", "1234/" + id}, {"tabRuntimeId", id}});
        batch.start(MeetingBatch::Operation::Refresh, targets);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        QCOMPARE(done.first().first().toInt(), 0);
        QCOMPARE(started.size(), 3);
        QCOMPARE(results.size(), 3);
        const auto first = results.at(0).first().toJsonObject();
        const auto failed = results.at(1).first().toJsonObject();
        const auto last = results.at(2).first().toJsonObject();
        QCOMPARE(first.value("requestKey").toString(), "1234/first");
        QVERIFY(first.value("verified").toBool());
        QCOMPARE(failed.value("requestKey").toString(), failedKey);
        QVERIFY(!failed.value("ok").toBool());
        QVERIFY(!failed.value("verified").toBool());
        QCOMPARE(failed.value("timedOut").toBool(), fault == "stall");
        QCOMPARE(window.meetingCountValue_->text(), "0/1");
        QCOMPARE(window.meetings_.first().microphone, ManagedMeeting::Microphone::Unknown);
        QCOMPARE(window.meetingCards_.first()->findChild<QPushButton*>("soundButton")->text(), "Read Sound");
        QCOMPARE(last.value("requestKey").toString(), "1234/last");
        QVERIFY(last.value("verified").toBool());
        QVERIFY(first.value("fixturePid") != last.value("fixturePid"));
    }

    void releaseWorkerRepliesWhileItsInputStaysOpen()
    {
        QProcess worker;
        worker.start(QString::fromUtf8(MEETING_APP_PATH), {"--meeting-batch-worker"});
        QVERIFY2(worker.waitForStarted(3000), qPrintable(worker.errorString()));
        for (const QString& handle : QStringList{"0", "ffffffffffffffff"})
        {
            const QJsonObject request{
                {"operation", static_cast<int>(MeetingBatch::Operation::Refresh)},
                {"windowHandle", handle}, {"requestKey", handle}, {"tabRuntimeId", "missing"}
            };
            const auto line = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
            QCOMPARE(worker.write(line), line.size());
            QDeadlineTimer deadline(5000);
            QJsonObject reply;
            bool initialized = false;
            while (reply.isEmpty())
            {
                QVERIFY(!deadline.hasExpired());
                if (!worker.canReadLine())
                {
                    QVERIFY2(worker.waitForReadyRead(static_cast<int>(deadline.remainingTime())),
                             qPrintable(worker.errorString()));
                    continue;
                }
                const auto message = QJsonDocument::fromJson(worker.readLine()).object();
                if (message.value("type").toString() == "initialized")
                    initialized = message.value("ok").toBool();
                else if (message.value("type").toString() == "result")
                    reply = message.value("result").toObject();
            }
            QCOMPARE(reply.value("requestKey").toString(), handle);
            QVERIFY(!reply.value("ok").toBool());
            if (handle != "0")
                QVERIFY(initialized);
        }
        worker.closeWriteChannel();
        QVERIFY(worker.waitForFinished(3000));
        QCOMPARE(worker.exitCode(), 0);
    }

    void doubleClickRequestsTheChromeTab()
    {
        MeetingControllerPane window;
        populate(window, ManagedMeeting::Sound::On, ManagedMeeting::Microphone::Muted);
        QSignalSpy spy(&window, &MeetingControllerPane::activationRequested);
        QTest::mouseDClick(window.meetingCards_.first(), Qt::LeftButton, Qt::NoModifier, QPoint(8, 8));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(2).toString(), QString("Audio test meeting"));
    }

    void meetingGridFollowsItsViewport()
    {
        MeetingControllerPane window;
        populate(window, ManagedMeeting::Sound::On, ManagedMeeting::Microphone::Muted);
        for (int i = 1; i <= 3; ++i)
        {
            auto meeting = window.meetings_.first();
            meeting.tabRuntimeId = QString::number(i);
            window.meetings_.append(meeting);
        }
        window.refreshMeetingCards();
        window.resize(1500, 720);
        window.show();
        const auto lastCell = [&window] {
            const int item = window.meetingsLayout_->indexOf(window.meetingCards_.last());
            if (item < 0) return QPoint(-1, -1);
            int row, column, rowSpan, columnSpan;
            window.meetingsLayout_->getItemPosition(item, &row, &column, &rowSpan, &columnSpan);
            return QPoint(column, row);
        };
        QTRY_COMPARE(lastCell(), QPoint(0, 3));
        QTRY_COMPARE(window.meetingsArea_->horizontalScrollBar()->maximum(), 0);
        window.resize(window.minimumWidth(), 720);
        QTRY_COMPARE(window.width(), window.minimumWidth());
        const auto bounds = [&window](QWidget* widget) {
            return QRect(widget->mapTo(&window, QPoint()), widget->size());
        };
        QTRY_VERIFY(bounds(window.dashboardPickUpButton_).bottom() < bounds(window.meetingsArea_).top());
        QCOMPARE(bounds(window.dashboardMuteAllButton_).top(), bounds(window.uiaRefreshButton_).top());
        QCOMPARE(bounds(window.dashboardSoundOffButton_).top(), bounds(window.dashboardPickUpButton_).top());
        QVERIFY(bounds(window.dashboardMuteAllButton_).bottom() < bounds(window.dashboardSoundOffButton_).top());
        for (auto* button : {window.dashboardMuteAllButton_, window.uiaRefreshButton_,
                             window.dashboardSoundOffButton_, window.dashboardPickUpButton_}) {
            for (auto* label : button->findChildren<QLabel*>()) {
                if (!label->isVisible()) continue;
                QVERIFY(bounds(button).contains(bounds(label)));
                const auto ink = label->fontMetrics().tightBoundingRect(label->text()).size();
                const auto space = label->contentsRect().size();
                QVERIFY2(space.width() >= ink.width() && space.height() >= ink.height(),
                         qPrintable(button->objectName() + '/' + label->objectName()));
            }
        }
        QVERIFY(window.meetingsArea_->viewport()->width() >= window.meetingCards_.first()->width());
        QTRY_COMPARE(lastCell(), QPoint(0, 3));
        QTRY_COMPARE(window.meetingCards_.first()->width(), window.meetingsArea_->viewport()->width());
        const int stackedLeft = bounds(window.meetingCards_.first()).left();
        const int stackedWidth = window.meetingCards_.first()->width();
        for (QFrame* card : window.meetingCards_) {
            QCOMPARE(bounds(card).left(), stackedLeft);
            QCOMPARE(card->width(), stackedWidth);
        }
        QVERIFY(bounds(window.meetingCards_.at(1)).top() >= bounds(window.meetingCards_.first()).bottom());
        window.resize(window.minimumWidth(), 360);
        QTRY_COMPARE(window.meetingsArea_->horizontalScrollBar()->maximum(), 0);
        QTRY_VERIFY(window.meetingsArea_->verticalScrollBar()->maximum() > 0);
        window.resize(1500, 720);
        QTRY_VERIFY(bounds(window.dashboardPickUpButton_).right() < bounds(window.meetingsArea_).left());
        QTRY_COMPARE(window.meetingsArea_->horizontalScrollBar()->maximum(), 0);
    }

    void rendersCardStates()
    {
        MeetingControllerPane window;
        populate(window, ManagedMeeting::Sound::On, ManagedMeeting::Microphone::Muted);
        auto disconnected = window.meetings_.first();
        disconnected.tabRuntimeId = "off-tab";
        disconnected.title = "Meeting with audio disconnected";
        disconnected.sound = ManagedMeeting::Sound::Off;
        disconnected.microphone = ManagedMeeting::Microphone::AudioNotConnected;
        window.meetings_.append(disconnected);
        auto unknown = disconnected;
        unknown.tabRuntimeId = "unknown-tab";
        unknown.title = "Meeting awaiting an audio read";
        unknown.sound = ManagedMeeting::Sound::Unknown;
        unknown.microphone = ManagedMeeting::Microphone::Unknown;
        window.meetings_.append(unknown);
        window.refreshMeetingCards();
        QCOMPARE(window.meetingCards_.at(0)->objectName(), QString("meetingCardActive"));
        QCOMPARE(window.meetingCards_.at(1)->objectName(), QString("meetingCardActive"));
        QCOMPARE(window.meetingCards_.at(2)->objectName(), QString("meetingCard"));
        QVERIFY(window.meetingCards_.at(0)->findChild<QLabel*>("activeBadge") != nullptr);
        QVERIFY(window.meetingCards_.at(1)->findChild<QLabel*>("activeBadge") != nullptr);
        QVERIFY(window.meetingCards_.at(2)->findChild<QLabel*>("activeBadge") == nullptr);
        QVERIFY(window.meetingCards_.at(0)->findChild<QPushButton*>("activeRefreshButton") != nullptr);
        QVERIFY(window.meetingCards_.at(2)->findChild<QPushButton*>("activeRefreshButton") == nullptr);
        QCOMPARE(window.meetingCountValue_->text(), QString("2/3"));
        window.show();
        QTest::qWait(100);
        auto* activeCard = window.meetingCards_.at(0);
        auto* refresh = activeCard->findChild<QPushButton*>("activeRefreshButton");
        auto* activeLabel = activeCard->findChild<QLabel*>("activeBadge");
        QVERIFY(refresh->mapTo(activeCard, QPoint()).x() < activeLabel->mapTo(activeCard, QPoint()).x());
        QPushButton* unmute = nullptr;
        for (QPushButton* button : activeCard->findChildren<QPushButton*>())
        {
            if (button->text().contains(QStringLiteral("Unmute")))
                unmute = button;
        }
        auto* sound = activeCard->findChild<QPushButton*>("soundButton");
        QVERIFY(unmute != nullptr);
        QVERIFY(qAbs(unmute->width() * 2 - sound->width() * 3) <= 8);
        const auto origin = [&window](QWidget* widget) {
            return widget->mapTo(&window, QPoint());
        };
        const QPoint mute = origin(window.dashboardMuteAllButton_);
        const QPoint soundOff = origin(window.dashboardSoundOffButton_);
        const QPoint connection = origin(window.uiaRefreshButton_);
        const QPoint pickup = origin(window.dashboardPickUpButton_);
        QVERIFY(mute.y() < connection.y());
        QVERIFY(connection.y() < soundOff.y());
        QVERIFY(soundOff.y() < pickup.y());
        QVERIFY(qAbs(mute.x() - pickup.x()) <= 2);
        QVERIFY(mute.x() < window.meetingsArea_->mapTo(&window, QPoint()).x());
        QCOMPARE(window.dashboardMuteAllButton_->findChild<QLabel*>("railCaption")->text(), QString("Mute all"));
        QVERIFY(window.findChild<QPushButton*>("railCountButton") == nullptr);
        QCOMPARE(window.dashboardSoundOffButton_->findChild<QLabel*>("railCaption")->text(), QString("Sound off all"));
        QCOMPARE(window.uiaRefreshButton_->findChild<QLabel*>("railCaption")->text(), QString("Connection"));
        QCOMPARE(window.dashboardPickUpButton_->findChild<QLabel*>("railCaption")->text(), QString("Pick up tabs"));
        QCOMPARE(window.meetingCountValue_->text(), QString("2/3"));
        QVERIFY(window.collapseButton_->text().contains(QStringLiteral("Meetings (2/3)")));
        QVERIFY(!window.uiaValue_->isVisible());
        for (QLabel* label : window.findChildren<QLabel*>())
        {
            QVERIFY(!label->text().contains(QStringLiteral("Managed Meetings")));
            QVERIFY(!label->text().contains(QStringLiteral("Controls read Zoom")));
            QVERIFY(!label->isVisible() || !label->text().contains(QStringLiteral("Active / total")));
            QVERIFY(!label->isVisible() || label->text() != QStringLiteral("Idle"));
        }
        QVERIFY(window.grab().save("meeting-card-sound-states.png"));
        window.resize(1120, 720);
        QTest::qWait(100);
        QVERIFY(window.grab().save("meeting-card-sound-states-minimum.png"));
        for (int index = 0; index < window.meetingCards_.size(); ++index)
        {
            int row = -1;
            int column = -1;
            int rowSpan = 0;
            int columnSpan = 0;
            window.meetingsLayout_->getItemPosition(
                window.meetingsLayout_->indexOf(window.meetingCards_.at(index)),
                &row, &column, &rowSpan, &columnSpan);
            QCOMPARE(row, index);
            QCOMPARE(column, 0);
            QCOMPARE(window.meetingCards_.at(index)->width(), window.meetingsArea_->viewport()->width());
        }
    }
};

int main(int argc, char* argv[])
{
    if (argc == 2 && std::strcmp(argv[1], "--meeting-batch-worker") == 0)
    {
        QCoreApplication app(argc, argv);
        return runTestBatchWorker();
    }
    QApplication app(argc, argv);
    MeetingControllerTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "MeetingControllerTests.moc"
