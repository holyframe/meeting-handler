#include "MeetingControllerPane.h"
#include "CaptionTheme.h"
#include "MeetingAudioSummary.h"
#include "windows/UiaWorker.h"

#include <algorithm>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStyle>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QMetaObject>

namespace
{
constexpr int kMeetingCardWidth = 300;
constexpr int kContentMargin = 12;
constexpr int kMeetingsColumnMinimum = kMeetingCardWidth + 2 * kContentMargin;

QLabel* makeMutedLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName("mutedText");
    label->setWordWrap(true);
    return label;
}

QFrame* makePanel(QWidget* parent, const QString& objectName = "panel")
{
    auto* panel = new QFrame(parent);
    panel->setObjectName(objectName);
    return panel;
}

QLabel* hiddenLabel(const QString& text, const QString& objectName, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(objectName);
    label->hide();
    return label;
}

}

MeetingControllerPane::MeetingControllerPane(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("meetingControllerPane");
    setAttribute(Qt::WA_StyledBackground);
    setMinimumWidth(kMeetingsColumnMinimum);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* shell = new QVBoxLayout(this);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);

    collapseBar_ = new QFrame(this);
    collapseBar_->setObjectName("collapseBar");
    collapseBar_->setFixedHeight(caption_theme::paneHeaderHeight);
    auto* barLayout = new QHBoxLayout(collapseBar_);
    barLayout->setContentsMargins(8, 0, 8, 0);
    barLayout->setSpacing(6);
    collapseButton_ = new QPushButton(collapseBar_);
    collapseButton_->setObjectName("collapseMeetingsButton");
    collapseButton_->setCursor(Qt::PointingHandCursor);
    collapseButton_->setAccessibleName("Collapse meetings");
    collapseButton_->setToolTip("Hide the meeting controls");
    collapseButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    barLayout->addWidget(collapseButton_, 1);
    setFocusProxy(collapseButton_);

    const auto addHeaderButton = [this, barLayout](const QString& objectName, const QString& iconPath,
                                                  const QString& name) {
        auto* button = new QPushButton(collapseBar_);
        button->setObjectName(objectName);
        button->setProperty("meetingHeaderAction", true);
        button->setIcon(QIcon(iconPath));
        button->setIconSize(QSize(16, 16));
        button->setAccessibleName(name);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setFixedSize(24, 24);
        barLayout->addWidget(button, 0, Qt::AlignVCenter);
        return button;
    };
    dashboardMuteAllButton_ = addHeaderButton("muteAllMeetingsButton", ":/icons/microphone-off.svg",
                                              tr("Mute all meetings"));
    dashboardMuteAllButton_->setEnabled(false);
    dashboardSoundOffButton_ = addHeaderButton("soundOffAllMeetingsButton", ":/icons/speaker-off.svg",
                                               tr("Sound off all meetings"));
    dashboardSoundOffButton_->setEnabled(false);
    dashboardPickUpButton_ = addHeaderButton("importMeetingTabsButton", ":/icons/import-meeting-tabs.svg",
                                             tr("Pick up meeting tabs"));
    dashboardPickUpButton_->setToolTip(QStringLiteral("Pick up one or more Chrome meeting tabs."));

    uiaRefreshButton_ = new QPushButton(collapseBar_);
    uiaRefreshButton_->setObjectName("railConnectionButton");
    uiaRefreshButton_->setCursor(Qt::PointingHandCursor);
    uiaRefreshButton_->setFocusPolicy(Qt::StrongFocus);
    uiaRefreshButton_->setAccessibleName("Connection status");
    uiaRefreshButton_->setProperty("connection", "idle");
    uiaRefreshButton_->setEnabled(false);
    uiaRefreshButton_->setFixedSize(10, 10);
    barLayout->addWidget(uiaRefreshButton_, 0, Qt::AlignVCenter);
    uiaValue_ = hiddenLabel("Idle", "railValue", uiaRefreshButton_);
    uiaDetail_ = hiddenLabel("Starts on first inspection", "railDetail", uiaRefreshButton_);

    dashboardPage_ = createDashboardPage();
    refreshCollapseTitle();
    shell->addWidget(collapseBar_);
    shell->addWidget(dashboardPage_, 1);
    logPanel_ = createDiagnosticsPage();
    logPanel_->hide();

    connect(collapseButton_, &QPushButton::clicked, this, [this] {
        setMeetingsCollapsed(true);
    });

    connect(dashboardPickUpButton_, &QPushButton::clicked,
            this, &MeetingControllerPane::requestMeetingTabImport);

    connect(inspectButton_, &QPushButton::clicked,
            this, &MeetingControllerPane::inspectSelectedWindow);

    connect(chromeWindowCombo_, &QComboBox::currentIndexChanged,
            this, [this](int index)
    {
        Q_UNUSED(index);
        inspectButton_->setEnabled(
            !chromeWindowCombo_->currentData().toString().isEmpty());
    });

    workerThread_ = new QThread(this);
    worker_ = new UiaWorker();
    worker_->moveToThread(workerThread_);

    connect(this, &MeetingControllerPane::scanRequested,
            worker_, &UiaWorker::scanChromeWindows, Qt::QueuedConnection);
    connect(this, &MeetingControllerPane::inspectRequested,
            worker_, &UiaWorker::inspectWindow, Qt::QueuedConnection);
    connect(this, &MeetingControllerPane::pickupRequested,
            worker_, &UiaWorker::listChromeTabs, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::chromeTabsListed,
            this, &MeetingControllerPane::onChromeTabsListed, Qt::QueuedConnection);
    connect(this, &MeetingControllerPane::activationRequested,
            worker_, &UiaWorker::activateMeeting, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::meetingActivated,
            this, &MeetingControllerPane::onMeetingActivated, Qt::QueuedConnection);
    connect(this, &MeetingControllerPane::microphoneReadRequested,
            worker_, &UiaWorker::readMicrophone, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::microphoneRead,
            this, &MeetingControllerPane::onMicrophoneRead, Qt::QueuedConnection);
    connect(this, &MeetingControllerPane::microphoneSetRequested,
            worker_, &UiaWorker::setMicrophoneMuted, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::microphoneSet,
            this, &MeetingControllerPane::onMicrophoneSet, Qt::QueuedConnection);
    connect(this, &MeetingControllerPane::audioReadRequested,
            worker_, &UiaWorker::readAudioConnection, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::audioConnectionRead,
            this, &MeetingControllerPane::onAudioConnectionRead, Qt::QueuedConnection);
    connect(this, &MeetingControllerPane::audioSetRequested,
            worker_, &UiaWorker::setAudioConnected, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::audioConnectionSet,
            this, &MeetingControllerPane::onAudioConnectionSet, Qt::QueuedConnection);
    batch_ = new MeetingBatch(this);
    connect(batch_, &MeetingBatch::meetingStarted, this, [this](int number, int total, const QString& key) {
        const auto* meeting = findMeeting(key);
        const QString progress = QString("Checking meeting %1 of %2").arg(number).arg(total);
        batchProgress_->setText(progress);
        batchProgress_->setToolTip(meeting ? meeting->title : QString());
        batchRow_->show();
        batchProgress_->show();
        cancelRemaining_->setEnabled(number < total);
        cancelRemaining_->show();
        setStatusMessage(meeting ? progress + " · " + meeting->title : progress);
    });
    connect(batch_, &MeetingBatch::meetingFinished, this, &MeetingControllerPane::onBatchMeetingFinished);
    connect(batch_, &MeetingBatch::finished, this, &MeetingControllerPane::onBatchFinished);
    connect(batch_, &MeetingBatch::initialized, this, &MeetingControllerPane::onInitialized);
    connect(batch_, &MeetingBatch::diagnostic, this, &MeetingControllerPane::appendLog);
    connect(cancelRemaining_, &QPushButton::clicked, this, [this] {
        batch_->cancelRemaining();
        cancelRemaining_->setEnabled(false);
        batchProgress_->setText("Finishing the current meeting");
        appendLog("Cancelled the remaining meetings in this batch.");
    });
    connect(this, &MeetingControllerPane::prepareImportedMeetingsRequested,
            worker_, &UiaWorker::prepareImportedMeetings, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::importedMeetingsPrepared,
            this, &MeetingControllerPane::onImportedMeetingsPrepared, Qt::QueuedConnection);

    connect(dashboardMuteAllButton_, &QPushButton::clicked,
            this, &MeetingControllerPane::requestMuteAll);
    connect(dashboardSoundOffButton_, &QPushButton::clicked,
            this, &MeetingControllerPane::requestSoundOffAll);
    connect(uiaRefreshButton_, &QPushButton::clicked,
            this, &MeetingControllerPane::requestRefreshConnections);
    connect(worker_, &UiaWorker::initialized,
            this, &MeetingControllerPane::onInitialized, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::chromeWindowsFound,
            this, &MeetingControllerPane::onChromeWindowsFound, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::inspectionFinished,
            this, &MeetingControllerPane::onInspectionFinished, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::diagnostic,
            this, &MeetingControllerPane::appendLog, Qt::QueuedConnection);
    connect(workerThread_, &QThread::finished,
            worker_, &QObject::deleteLater);

    setHeaderPalette(palette());
    setStatusMessage(
        "Ready · Pick up meeting tabs to read microphone and sound states");
    workerThread_->start();
}

MeetingControllerPane::~MeetingControllerPane()
{
    delete batch_;
    if (workerThread_ && workerThread_->isRunning())
    {
        if (worker_)
        {
            QMetaObject::invokeMethod(
                worker_, "shutdown", Qt::BlockingQueuedConnection);
        }

        workerThread_->quit();
        workerThread_->wait();
    }
}

QWidget* MeetingControllerPane::createDashboardPage()
{
    auto* page = new QWidget(this);
    page->setObjectName("pageRoot");
    page->setAttribute(Qt::WA_StyledBackground);

    auto* contentLayout = new QVBoxLayout(page);
    contentLayout->setContentsMargins(kContentMargin, 8, kContentMargin, 8);
    contentLayout->setSpacing(8);
    contentLayout->setAlignment(Qt::AlignTop);

    meetingCountValue_ = hiddenLabel("0/0", "railValue", page);
    meetingCountDetail_ = hiddenLabel("Active / total", "railDetail", page);
    meetingCountLabel_ = hiddenLabel("0", "countPill", page);

    audioSummary_ = new MeetingAudioSummary(page);
    connect(audioSummary_, &MeetingAudioSummary::meetingNameDoubleClicked, this, [this](const QString& key) {
        if (cardOrder_.isEmpty())
            return;
        moveMeeting(key, meetings_.at(cardOrder_.first()).key());
        auto* scroll = meetingsArea_->verticalScrollBar();
        scroll->setValue(scroll->minimum());
    });
    contentLayout->addWidget(audioSummary_);

    batchRow_ = new QWidget(page);
    batchRow_->setObjectName("batchRow");
    auto* meetingHeader = new QHBoxLayout(batchRow_);
    meetingHeader->setContentsMargins(0, 0, 0, 0);
    meetingHeader->setSpacing(8);
    batchProgress_ = new QLabel(batchRow_);
    batchProgress_->setObjectName("batchProgress");
    batchProgress_->setWordWrap(true);
    cancelRemaining_ = new QPushButton("Cancel remaining", batchRow_);
    cancelRemaining_->setObjectName("secondaryButton");
    cancelRemaining_->setToolTip("Finish the current meeting and skip the remaining meetings.");
    meetingHeader->addWidget(batchProgress_, 1);
    meetingHeader->addWidget(cancelRemaining_);
    batchRow_->hide();
    contentLayout->addWidget(batchRow_);

    auto* meetingsArea = new QScrollArea(page);
    meetingsArea_ = meetingsArea;
    meetingsArea->setObjectName("meetingsScrollArea");
    meetingsArea->setWidgetResizable(true);
    meetingsArea->setFrameShape(QFrame::NoFrame);
    meetingsArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    meetingsArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    auto* meetingsContent = new QWidget(meetingsArea);
    meetingsContent->setObjectName("meetingsContent");
    meetingsContent->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    auto* meetingsLayout = new QGridLayout(meetingsContent);
    meetingsLayout->setSizeConstraint(QLayout::SetNoConstraint);
    meetingsLayout->setContentsMargins(0, 0, 0, 0);
    meetingsLayout->setHorizontalSpacing(0);
    meetingsLayout->setVerticalSpacing(12);
    meetingsLayout->setAlignment(Qt::AlignTop);
    meetingsLayout_ = meetingsLayout;

    auto* emptyCard = makePanel(meetingsContent, "emptyMeetingsCard");
    meetingEmptyCard_ = emptyCard;
    auto* emptyLayout = new QVBoxLayout(emptyCard);
    emptyLayout->setSizeConstraint(QLayout::SetNoConstraint);
    emptyLayout->setContentsMargins(16, 10, 16, 10);
    emptyLayout->setSpacing(4);
    emptyLayout->setAlignment(Qt::AlignCenter);

    auto* emptyIcon = new QLabel("▣", emptyCard);
    emptyIcon->setObjectName("emptyIcon");
    emptyIcon->setAlignment(Qt::AlignCenter);
    auto* emptyTitle = new QLabel("No meetings picked up", emptyCard);
    emptyTitle->setObjectName("emptyTitle");
    emptyTitle->setAlignment(Qt::AlignCenter);
    auto* emptyDescription = makeMutedLabel("Select + to import Zoom meeting tabs from Chrome.", emptyCard);
    emptyDescription->setAlignment(Qt::AlignCenter);
    emptyDescription->setMinimumWidth(0);
    emptyDescription->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    emptyLayout->addWidget(emptyIcon);
    emptyLayout->addWidget(emptyTitle);
    emptyLayout->addWidget(emptyDescription);

    meetingsArea->setWidget(meetingsContent);
    meetingsArea->viewport()->installEventFilter(this);
    contentLayout->addWidget(meetingsArea, 1);
    layoutMeetingCards();
    refreshActionTooltips();

    return page;
}

QWidget* MeetingControllerPane::createDiagnosticsPage()
{
    auto* page = new QWidget(this);
    page->setObjectName("appLogPanel");
    page->setAttribute(Qt::WA_StyledBackground);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 22, 28, 20);
    layout->setSpacing(16);

    auto* title = new QLabel("Log", page);
    title->setObjectName("sectionTitle");
    auto* subtitle = makeMutedLabel(
        "Caption actions, meeting actions, and UI Automation events are kept here.",
        page);
    subtitle->setObjectName("pageSubtitle");
    layout->addWidget(title);
    layout->addWidget(subtitle);

    auto* browserPanel = makePanel(page, "panel");
    auto* browserLayout = new QVBoxLayout(browserPanel);
    browserLayout->setContentsMargins(16, 14, 16, 14);
    browserLayout->setSpacing(10);

    auto* browserTitle = new QLabel("CHROME WINDOW INSPECTOR", browserPanel);
    browserTitle->setObjectName("panelHeading");
    browserLayout->addWidget(browserTitle);

    auto* browserLabel = new QLabel("Chrome window", browserPanel);
    browserLabel->setObjectName("mutedText");

    chromeWindowCombo_ = new QComboBox(browserPanel);
    chromeWindowCombo_->setObjectName("chromeWindowCombo");
    chromeWindowCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    chromeWindowCombo_->setMinimumContentsLength(22);
    browserLabel->setBuddy(chromeWindowCombo_);
    chromeWindowCombo_->addItem("Click Refresh to list Chrome windows", QString());

    auto* refreshButton = new QPushButton("⟳  Refresh", browserPanel);
    refreshButton->setObjectName("secondaryButton");
    refreshButton->setMinimumHeight(40);

    inspectButton_ = new QPushButton("Inspect Selected Window", browserPanel);
    inspectButton_->setObjectName("secondaryButton");
    inspectButton_->setMinimumHeight(40);
    inspectButton_->setEnabled(false);

    auto* selector = new QHBoxLayout;
    selector->addWidget(browserLabel);
    selector->addWidget(chromeWindowCombo_, 1);
    auto* actions = new QHBoxLayout;
    actions->addWidget(refreshButton);
    actions->addWidget(inspectButton_);
    actions->addStretch();
    auto* controls = new QFormLayout;
    controls->setRowWrapPolicy(QFormLayout::WrapLongRows);
    controls->setLayout(0, QFormLayout::LabelRole, selector);
    controls->setLayout(0, QFormLayout::FieldRole, actions);
    browserLayout->addLayout(controls);
    layout->addWidget(browserPanel);

    diagnosticsTabs_ = new QTabWidget(page);
    diagnosticsTabs_->setObjectName("diagnosticsTabs");

    liveDiagnostics_ = new QPlainTextEdit(diagnosticsTabs_);
    liveDiagnostics_->setObjectName("appLog");
    liveDiagnostics_->setReadOnly(true);
    liveDiagnostics_->setPlaceholderText(
        "Caption actions, meeting actions, and UI Automation events appear here.");

    accessibilityTree_ = new QPlainTextEdit(diagnosticsTabs_);
    accessibilityTree_->setObjectName("diagnosticsText");
    accessibilityTree_->setReadOnly(true);
    accessibilityTree_->setPlaceholderText(
        "Inspect a Chrome window to view its UI Automation accessibility tree.");

    diagnosticsTabs_->addTab(liveDiagnostics_, "Log");
    diagnosticsTabs_->addTab(accessibilityTree_, "Accessibility Tree");
    layout->addWidget(diagnosticsTabs_, 1);

    auto* note = makeMutedLabel(
        "Inspection reads the accessibility tree and does not change the meeting.",
        page);
    layout->addWidget(note);

    connect(refreshButton, &QPushButton::clicked,
            this, &MeetingControllerPane::refreshChromeWindows);

    return page;
}

void MeetingControllerPane::setLogVisible(bool visible)
{
    if (!logPanel_ || logPanel_->isVisible() == visible)
        return;
    logPanel_->setVisible(visible);
    emit logVisibilityChanged(visible);
}

void MeetingControllerPane::setMeetingsCollapsed(bool collapsed)
{
    if (meetingsCollapsed_ == collapsed)
        return;
    emit meetingsCollapseAboutToChange();
    meetingsCollapsed_ = collapsed;
    setVisible(!collapsed);
    emit meetingsCollapsedChanged(collapsed);
}

void MeetingControllerPane::onInitialized(bool success, const QString& message)
{
    updateUiaStatus(success, message);
}

void MeetingControllerPane::updateUiaStatus(bool ready, const QString& message)
{
    const QString state = ready ? "Ready" : "Unavailable";
    uiaValue_->setText(state);
    uiaDetail_->setText(ready
        ? "Windows UI Automation initialized"
        : "Initialization failed");
    refreshActionTooltips();

    if (batchProgress_->isHidden())
        setStatusMessage(message);
    appendLog(message);
}

void MeetingControllerPane::refreshChromeWindows()
{
    if (scanInProgress_)
        return;

    scanInProgress_ = true;
    setStatusMessage("Scanning visible Chrome windows...");
    appendLog("Chrome window scan started.");
    emit scanRequested();
}

void MeetingControllerPane::onChromeWindowsFound(const QJsonArray& windows)
{
    scanInProgress_ = false;

    chromeWindowCombo_->clear();
    if (windows.isEmpty())
    {
        chromeWindowCombo_->addItem("No Chrome windows detected", QString());
    }
    else
    {
        for (const QJsonValue& value : windows)
        {
            const QJsonObject item = value.toObject();
            const QString title = item.value("title").toString().isEmpty()
                ? "Chrome window"
                : item.value("title").toString();
            const QString hwnd = item.value("hwnd").toString();
            const QString profile = item.value("profileName").toString();
            const QString label = profile.isEmpty()
                ? QString("%1  ·  PID %2").arg(title).arg(item.value("processId").toInteger())
                : QString("%1  ·  %2").arg(title, profile);
            chromeWindowCombo_->addItem(label, hwnd);
        }
    }

    const int count = windows.size();
    inspectButton_->setEnabled(
        !chromeWindowCombo_->currentData().toString().isEmpty());

    setStatusMessage(
        QString("Chrome scan complete · %1 visible window(s) detected").arg(count));
    appendLog(QString("Chrome window scan completed: %1 visible window(s) found.")
                  .arg(count));
}

quintptr MeetingControllerPane::selectedWindowHandle() const
{
    bool ok = false;
    const qulonglong value =
        chromeWindowCombo_->currentData().toString().toULongLong(&ok, 16);
    return ok ? static_cast<quintptr>(value) : 0;
}

void MeetingControllerPane::inspectSelectedWindow()
{
    const quintptr hwnd = selectedWindowHandle();
    if (!hwnd)
    {
        appendLog("Inspection skipped: no Chrome window is selected.");
        return;
    }

    inspectButton_->setEnabled(false);
    setStatusMessage("Inspecting selected Chrome window through UI Automation...");
    appendLog("UIA inspection requested for HWND 0x"
        + QString::number(static_cast<qulonglong>(hwnd), 16).toUpper());
    emit inspectRequested(hwnd);
}

void MeetingControllerPane::onInspectionFinished(const QJsonObject& result)
{
    accessibilityTree_->setPlainText(
        QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Indented)));
    diagnosticsTabs_->setCurrentIndex(1);
    setLogVisible(true);

    if (result.value("ok").toBool())
    {
        const int count = result.value("visitedNodes").toInt();
        setStatusMessage(
            QString("UIA inspection complete · %1 nodes visited").arg(count));
        appendLog(QString("UIA inspection completed: %1 nodes visited.").arg(count));
    }
    else
    {
        const QString error = result.value("error").toString();
        setStatusMessage("UIA inspection failed");
        appendLog("UIA inspection failed: " + error);
    }

    inspectButton_->setEnabled(
        !chromeWindowCombo_->currentData().toString().isEmpty());
}

namespace
{
QString tabDetailText(const QJsonObject& tab)
{
    QStringList parts;

    const QString id = tab.value("meetingId").toString();
    if (!id.isEmpty())
        parts.append(QString("Zoom meeting %1").arg(id));
    else if (tab.value("addressKnown").toBool())
        parts.append(tab.value("host").toString());
    else
        parts.append("Identified by Zoom tab title");

    if (tab.value("selected").toBool())
        parts.append("Shown in this window");

    return parts.join("   ·   ");
}

QList<int> askTabsToImport(QWidget* parent, const QJsonArray& tabs)
{
    QDialog dialog(parent);
    dialog.setObjectName("tabImportDialog");
    dialog.setWindowTitle("Import meeting tabs");
    dialog.setModal(true);
    dialog.resize(760, 560);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);

    auto* intro = new QLabel(
        "Select Zoom meeting tabs to import from Chrome. If a meeting is missing, "
        "show its tab in Chrome and open this picker again.",
        &dialog);
    intro->setObjectName("settingsBody");
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* listHost = new QWidget(scroll);
    auto* listLayout = new QVBoxLayout(listHost);
    listLayout->setContentsMargins(0, 0, 8, 0);
    listLayout->setSpacing(8);

    QList<QCheckBox*> boxes;
    boxes.reserve(tabs.size());

    QStringList profileOrder;
    QHash<QString, QList<int>> tabsByProfile;
    for (int index = 0; index < tabs.size(); ++index)
    {
        const QString profileKey = tabs.at(index).toObject().value("profileKey").toString();
        if (!tabsByProfile.contains(profileKey))
            profileOrder.append(profileKey);
        tabsByProfile[profileKey].append(index);
    }

    for (const QString& profileKey : profileOrder)
    {
        const QJsonObject first =
            tabs.at(tabsByProfile.value(profileKey).first()).toObject();
        const QString profileName = first.value("profileName").toString().isEmpty()
            ? QString("Google Chrome")
            : first.value("profileName").toString();

        auto* heading = new QLabel(profileName, listHost);
        heading->setObjectName("profileHeading");
        listLayout->addWidget(heading);

        for (int index : tabsByProfile.value(profileKey))
        {
        const QJsonObject tab = tabs.at(index).toObject();
        auto* row = new QWidget(listHost);
        row->setObjectName("tabImportRow");
        auto* rowLayout = new QVBoxLayout(row);
        rowLayout->setContentsMargins(12, 10, 12, 10);
        rowLayout->setSpacing(2);

        const QString name = tab.value("tabName").toString().isEmpty()
            ? QString("Untitled tab")
            : tab.value("tabName").toString();

        auto* box = new QCheckBox(name, row);
        box->setObjectName("tabImportCheck");
        box->setProperty("tabIndex", index);
        auto* detail = new QLabel(tabDetailText(tab), row);
        detail->setObjectName("mutedText");
        detail->setWordWrap(true);
        detail->setContentsMargins(26, 0, 0, 0);

        rowLayout->addWidget(box);
        rowLayout->addWidget(detail);
        listLayout->addWidget(row);
        boxes.append(box);
        }
    }

    listLayout->addStretch();
    scroll->setWidget(listHost);
    layout->addWidget(scroll, 1);

    auto* buttons = new QHBoxLayout();
    auto* selectAll = new QPushButton("Select all", &dialog);
    auto* clear = new QPushButton("Clear", &dialog);
    auto* cancel = new QPushButton("Cancel", &dialog);
    auto* import = new QPushButton("Import", &dialog);
    import->setObjectName("primaryButton");
    import->setDefault(true);
    import->setEnabled(false);

    buttons->addWidget(selectAll);
    buttons->addWidget(clear);
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(import);
    layout->addLayout(buttons);

    const auto updateImportEnabled = [boxes, import]
    {
        const bool anyChecked = std::any_of(
            boxes.cbegin(), boxes.cend(),
            [](const QCheckBox* box) { return box->isChecked(); });
        import->setEnabled(anyChecked);
    };

    for (QCheckBox* box : boxes)
        QObject::connect(box, &QCheckBox::toggled, &dialog, updateImportEnabled);

    QObject::connect(selectAll, &QPushButton::clicked, &dialog, [boxes, updateImportEnabled]
    {
        for (QCheckBox* box : boxes)
            box->setChecked(true);
        updateImportEnabled();
    });
    QObject::connect(clear, &QPushButton::clicked, &dialog, [boxes, updateImportEnabled]
    {
        for (QCheckBox* box : boxes)
            box->setChecked(false);
        updateImportEnabled();
    });
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(import, &QPushButton::clicked, &dialog, &QDialog::accept);

    if (dialog.exec() != QDialog::Accepted)
        return {};

    QList<int> selected;
    for (const QCheckBox* box : boxes)
    {
        if (box->isChecked())
            selected.append(box->property("tabIndex").toInt());
    }
    return selected;
}
}

void MeetingControllerPane::requestMeetingTabImport()
{
    if (pickupInProgress_ || meetingBusy_)
        return;

    pickupInProgress_ = true;
    dashboardPickUpButton_->setEnabled(false);
    setStatusMessage("Reading Chrome tabs through UI Automation...");
    appendLog("Chrome tab list requested.");
    emit pickupRequested();
}

void MeetingControllerPane::onChromeTabsListed(const QJsonObject& result)
{
    pickupInProgress_ = false;
    dashboardPickUpButton_->setEnabled(true);

    if (!result.value("ok").toBool())
    {
        const QString error = result.value("error").toString();
        setStatusMessage("Could not list Chrome tabs");
        appendLog("Chrome tab list failed: " + error);
        QMessageBox::warning(this, "Could not list Chrome tabs", error);
        return;
    }

    const QJsonArray chromeTabs = result.value("tabs").toArray();
    QJsonArray tabs;
    for (const auto& tab : chromeTabs) {
        if (tab.toObject().value("zoomMeeting").toBool())
            tabs.append(tab);
    }
    appendLog(QString("Found %1 Zoom meeting tab(s) across %2 Chrome window(s).")
                  .arg(tabs.size())
                  .arg(result.value("windowCount").toInt()));
    if (tabs.isEmpty()) {
        setStatusMessage(tr("No Zoom meeting tabs found"));
        QMessageBox::information(this, tr("Import meeting tabs"),
            tr("No Zoom meeting tabs found. Show a Zoom meeting in Chrome and try again."));
        return;
    }

    const QList<int> selected = askTabsToImport(this, tabs);
    if (selected.isEmpty())
    {
        setStatusMessage("Tab import cancelled");
        appendLog("Tab import cancelled.");
        return;
    }

    int imported = 0;
    int skipped = 0;
    QJsonArray targets;

    for (int index : selected)
    {
        const QJsonObject tab = tabs.at(index).toObject();

        const ManagedMeeting meeting = ManagedMeeting::fromTab(tab);
        const bool duplicate = std::any_of(meetings_.cbegin(), meetings_.cend(),
            [&meeting](const ManagedMeeting& existing) { return meeting.matches(existing); });

        if (duplicate)
        {
            ++skipped;
            appendLog(QString("Skipped \"%1\": already managed.").arg(meeting.title));
            continue;
        }

        meetings_.append(meeting);
        ++imported;
        targets.append(meeting.automationTarget());
        appendLog(QString("Imported \"%1\" from %2.")
                      .arg(meeting.title,
                           meeting.profileName.isEmpty()
                               ? QString("Google Chrome")
                               : meeting.profileName));
    }

    if (imported == 0)
    {
        setStatusMessage(
            QString("Imported 0 tab(s) · %1 already managed").arg(skipped));
        return;
    }

    appendLog(QString("Opening %1 imported meeting(s) and reading the microphone.")
                  .arg(imported));
    setMeetingBusy(true, "Opening the meeting and reading the microphone...");
    emit prepareImportedMeetingsRequested(targets);
}

void MeetingControllerPane::onImportedMeetingsPrepared(const QJsonArray& results)
{
    QStringList failures;

    for (const QJsonValue& value : results)
    {
        const QJsonObject result = value.toObject();
        ManagedMeeting* meeting = findMeeting(result.value("requestKey").toString());
        if (!meeting)
            continue;

        meeting->applyObservation(result);

        if (!result.value("ok").toBool() || result.contains("audioError"))
        {
            const QString error = result.contains("audioError")
                ? result.value("audioError").toString() : result.value("error").toString();
            failures.append(QString("%1: %2")
                                .arg(meeting->title, error));
            appendLog(QString("Could not prepare \"%1\": %2")
                          .arg(meeting->title, error));
        }
        else
        {
            appendLog(QString("\"%1\" microphone is %2; computer audio is %3.")
                          .arg(meeting->title, result.value("state").toString(),
                               result.value("audioState").toString()));
        }
    }

    setMeetingBusy(false, failures.isEmpty()
        ? "Imported meetings; microphone and sound states were read"
        : "Imported meetings, with some audio reads failing");

    if (!failures.isEmpty())
    {
        QMessageBox::warning(
        this,
            "Some meetings could not be read",
            failures.join("\n\n"));
    }
}

QFrame* MeetingControllerPane::createMeetingCard(const ManagedMeeting& meeting)
{
    const QString key = meeting.key();
    const bool isActive = meeting.hasKnownSound();

    auto* card = makePanel(
        meetingsLayout_->parentWidget(), isActive ? "meetingCardActive" : "meetingCard");
    auto* layout = new QVBoxLayout(card);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    layout->setContentsMargins(12, 14, 12, 14);
    layout->setSpacing(10);

    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(11);

    auto* iconTile = new QLabel("▣", card);
    iconTile->setObjectName("meetingIcon");
    iconTile->setAlignment(Qt::AlignCenter);
    iconTile->setFixedSize(38, 38);

    auto* title = new QLineEdit(meeting.displayTitle(), card);
    title->setCursorPosition(0);
    title->setObjectName("meetingTitle");
    title->setMinimumWidth(0);
    title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    title->setFrame(false);
    title->setPlaceholderText("Meeting name");
    title->setToolTip("Name shown on this card. The Chrome tab title is not changed.");
    title->setClearButtonEnabled(false);
    connect(title, &QLineEdit::editingFinished, this, [this, key, title]
    {
        if (ManagedMeeting* current = findMeeting(key))
        {
            QString name = title->text().trimmed();
            if (name.isEmpty())
            {
                name = current->chromeTabName.isEmpty()
                    ? QStringLiteral("Zoom meeting") : current->chromeTabName;
                title->setText(name);
            }
            if (current->title != name)
            {
                current->title = name;
                audioSummary_->setMeetings(meetings_);
                appendLog(QString("Renamed a meeting card to \"%1\".").arg(name));
            }
        }
    });

    headerRow->addWidget(iconTile, 0, Qt::AlignVCenter);
    headerRow->addWidget(title, 1, Qt::AlignVCenter);
    iconTile->setAttribute(Qt::WA_TransparentForMouseEvents);

    if (isActive)
    {
        auto* badge = new QWidget(card);
        badge->setObjectName("activeBadgeShell");
        auto* badgeLayout = new QHBoxLayout(badge);
        badgeLayout->setContentsMargins(3, 1, 8, 1);
        badgeLayout->setSpacing(2);

        auto* refreshButton = new QPushButton(QStringLiteral("⟳"), badge);
        refreshButton->setObjectName("activeRefreshButton");
        refreshButton->setFixedSize(18, 18);
        refreshButton->setCursor(Qt::PointingHandCursor);
        refreshButton->setToolTip("Read the microphone and sound states from Zoom again.");
        refreshButton->setAccessibleName("Refresh meeting");
        refreshButton->setEnabled(!meetingBusy_);
        connect(refreshButton, &QPushButton::clicked,
                this, [this, key] { readSound(key); });

        auto* badgeText = new QLabel("Active", badge);
        badgeText->setObjectName("activeBadge");
        badgeText->setAttribute(Qt::WA_TransparentForMouseEvents);

        badgeLayout->addWidget(refreshButton);
        badgeLayout->addWidget(badgeText);
        headerRow->addWidget(badge, 0, Qt::AlignVCenter);
    }

    auto* closeButton = new QPushButton(QStringLiteral("×"), card);
    closeButton->setObjectName("cardCloseButton");
    closeButton->setFixedSize(28, 28);
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setToolTip("Remove this meeting from the list. Zoom is not changed.");
    closeButton->setAccessibleName("Remove meeting");
    closeButton->setEnabled(!meetingBusy_);
    connect(closeButton, &QPushButton::clicked,
            this, [this, key] { removeMeeting(key); });
    headerRow->addWidget(closeButton, 0, Qt::AlignVCenter);

    layout->addLayout(headerRow);

    auto* actionRow = new QHBoxLayout();
    actionRow->setSpacing(8);

    const auto addMicrophoneButton = [this, card, actionRow, key](const QString& label, bool mute)
    {
        auto* button = new QPushButton(label, card);
        button->setObjectName(mute ? "dangerButton" : "secondaryButton");
        button->setMinimumWidth(0);
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        button->setEnabled(!meetingBusy_);
        connect(button, &QPushButton::clicked,
                this, [this, key, mute] { setMicrophone(key, mute); });
        actionRow->addWidget(button, 3);
    };

    if (meeting.microphone == ManagedMeeting::Microphone::Muted)
        addMicrophoneButton("◉  Unmute", false);
    else if (meeting.microphone == ManagedMeeting::Microphone::Unmuted)
        addMicrophoneButton("◉  Mute", true);
    else if (meeting.microphone != ManagedMeeting::Microphone::AudioNotConnected)
    {
        addMicrophoneButton("◉  Mute", true);
        addMicrophoneButton("◉  Unmute", false);
    }

    auto* soundButton = new QPushButton(card);
    soundButton->setObjectName("soundButton");
    soundButton->setMinimumWidth(0);
    soundButton->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    soundButton->setEnabled(!meetingBusy_);
    if (meeting.sound == ManagedMeeting::Sound::Unknown)
    {
        soundButton->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
        soundButton->setText("Read Sound");
        soundButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
        soundButton->setToolTip("Read Zoom's computer-audio state before choosing Sound On or Off.");
        connect(soundButton, &QPushButton::clicked,
                this, [this, key] { readSound(key); });
    }
    else
    {
        const bool turnOn = meeting.sound == ManagedMeeting::Sound::Off;
        soundButton->setText(turnOn ? "Sound On" : "Sound Off");
        soundButton->setIcon(style()->standardIcon(
            turnOn ? QStyle::SP_MediaVolume : QStyle::SP_MediaVolumeMuted));
        soundButton->setToolTip(turnOn
            ? "Join computer audio, then read back the connection and microphone states."
            : "Leave computer audio to turn sound off. This disconnects the microphone too.");
        connect(soundButton, &QPushButton::clicked,
                this, [this, key, turnOn] { setSound(key, turnOn); });
    }

    actionRow->addWidget(soundButton, 2);
    layout->addLayout(actionRow);

    card->setProperty("meetingKey", key);
    card->setCursor(Qt::PointingHandCursor);
    card->setToolTip("Double-click to bring this Chrome tab forward. Drag to change the order.");
    card->setAttribute(Qt::WA_Hover, true);
    card->installEventFilter(this);
    for (QWidget* child : card->findChildren<QWidget*>())
    {
        child->setAttribute(Qt::WA_Hover, true);
        child->installEventFilter(this);
    }

    return card;
}

void MeetingControllerPane::refreshMeetingCards()
{
    clearDrag();

    for (QFrame* card : std::as_const(meetingCards_))
    {
        meetingsLayout_->removeWidget(card);
        card->deleteLater();
    }
    meetingCards_.clear();

    meetingEmptyCard_->setVisible(meetings_.isEmpty());

    while (cardOrder_.size() < meetings_.size())
        cardOrder_.append(static_cast<int>(cardOrder_.size()));
    for (int row : std::as_const(cardOrder_))
    {
        QFrame* card = createMeetingCard(meetings_.at(row));
        meetingCards_.append(card);
    }
    audioSummary_->setMeetings(meetings_);
    layoutMeetingCards();
    updateMeetingCounts();

    const bool canUseMeetings = !meetings_.isEmpty() && !meetingBusy_;
    dashboardMuteAllButton_->setEnabled(canUseMeetings);
    dashboardSoundOffButton_->setEnabled(canUseMeetings);
    uiaRefreshButton_->setEnabled(canUseMeetings);
    refreshActionTooltips();
}

void MeetingControllerPane::refreshActionTooltips()
{
    const QString needMeetings = QStringLiteral("Pick up a meeting tab first.");
    dashboardMuteAllButton_->setToolTip(dashboardMuteAllButton_->accessibleName() + '\n' +
        (meetings_.isEmpty() ? needMeetings
            : tr("Switches to each meeting tab and verifies that its microphone is muted.")));
    dashboardSoundOffButton_->setToolTip(dashboardSoundOffButton_->accessibleName() + '\n' +
        (meetings_.isEmpty() ? needMeetings
            : tr("Switches to each meeting tab and disconnects its Zoom computer audio, including the microphone.")));
    const QString connection = QString("Connection status · %1\n%2")
                                   .arg(uiaValue_->text(), uiaDetail_->text());
    uiaRefreshButton_->setToolTip(meetings_.isEmpty()
        ? connection + "\n" + needMeetings
        : connection + "\nRead every meeting's computer-audio connection and update which cards are active. "
                       "Chrome switches to each meeting tab to read it.");
    uiaRefreshButton_->setProperty(
        "connection",
        uiaValue_->text() == "Ready" ? "ready"
        : uiaValue_->text() == "Unavailable" ? "unavailable" : "idle");
    uiaRefreshButton_->style()->unpolish(uiaRefreshButton_);
    uiaRefreshButton_->style()->polish(uiaRefreshButton_);
}

void MeetingControllerPane::updateMeetingCounts()
{
    const auto active = std::count_if(meetings_.cbegin(), meetings_.cend(),
        [](const ManagedMeeting& meeting) { return meeting.hasKnownSound(); });
    meetingCountValue_->setText(QString("%1/%2").arg(active).arg(meetings_.size()));
    meetingCountDetail_->setText(meetings_.isEmpty() ? "No meetings picked up yet" : "Active / total");
    meetingCountLabel_->setText(QString::number(meetings_.size()));
    refreshCollapseTitle();
}

void MeetingControllerPane::refreshCollapseTitle()
{
    if (collapseButton_ && meetingCountValue_)
        collapseButton_->setText(QString("%1  Meetings (%2)")
                                      .arg(QStringLiteral("\u25C0"), meetingCountValue_->text()));
}

void MeetingControllerPane::refreshBatchCard(const QString& previousKey, const ManagedMeeting& meeting)
{
    for (int row = 0; row < meetingCards_.size(); ++row)
    {
        QFrame*& card = meetingCards_[row];
        if (card->property("meetingKey").toString() != previousKey)
            continue;
        QFrame* replacement = createMeetingCard(meeting);
        delete meetingsLayout_->replaceWidget(card, replacement);
        card->hide();
        card->deleteLater();
        card = replacement;
        replacement->show();
        audioSummary_->updateMeeting(cardOrder_.at(row), meeting);
        break;
    }
    fitMeetingCards();
    updateMeetingCounts();
}

ManagedMeeting* MeetingControllerPane::findMeeting(const QString& key)
{
    if (key.isEmpty())
        return nullptr;

    for (ManagedMeeting& meeting : meetings_)
    {
        if (meeting.key() == key)
            return &meeting;
    }

    return nullptr;
}

void MeetingControllerPane::layoutMeetingCards()
{
    const int count = meetingCards_.size();
    meetingCardColumns_ = 1;
    for (QFrame* card : std::as_const(meetingCards_))
        meetingsLayout_->removeWidget(card);
    meetingsLayout_->removeWidget(meetingEmptyCard_);
    for (int column = 0; column < meetingsLayout_->columnCount(); ++column)
        meetingsLayout_->setColumnStretch(column, 0);
    for (int row = 0; row < meetingsLayout_->rowCount(); ++row)
        meetingsLayout_->setRowStretch(row, 0);

    auto* content = meetingsLayout_->parentWidget();
    content->setMinimumWidth(0);
    content->setMaximumWidth(QWIDGETSIZE_MAX);
    if (count == 0)
    {
        meetingEmptyCard_->setMinimumWidth(0);
        meetingEmptyCard_->setMaximumWidth(QWIDGETSIZE_MAX);
        meetingEmptyCard_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        meetingsLayout_->setColumnStretch(0, 1);
        meetingsLayout_->addWidget(meetingEmptyCard_, 0, 0);
        meetingsLayout_->setRowStretch(1, 1);
    }
    else
    {
        for (int index = 0; index < count; ++index)
        {
            QFrame* card = meetingCards_.at(index);
            card->setMinimumWidth(0);
            card->setMaximumWidth(QWIDGETSIZE_MAX);
            card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
            meetingsLayout_->addWidget(card, index, 0);
        }
        meetingsLayout_->setColumnStretch(0, 1);
        meetingsLayout_->setRowStretch(count, 1);
    }
    syncMeetingsStrip();
    fitMeetingCards();
}

void MeetingControllerPane::fitMeetingCards()
{
    if (!meetingsArea_ || !meetingsLayout_)
        return;
    const int available = meetingsArea_->viewport()->width();
    if (available <= 0)
        return;
    const auto fit = [available](QWidget* widget) {
        if (!widget)
            return;
        widget->setFixedWidth(available);
    };
    if (meetingCards_.isEmpty())
        fit(meetingEmptyCard_);
    else {
        for (QFrame* card : std::as_const(meetingCards_))
            fit(card);
    }
}

void MeetingControllerPane::syncMeetingsStrip()
{
    if (!meetingsArea_)
        return;
    meetingsArea_->setMinimumHeight(0);
    meetingsArea_->setMaximumHeight(QWIDGETSIZE_MAX);
}

QFrame* MeetingControllerPane::meetingCardFor(QWidget* widget) const
{
    while (widget)
    {
        if (auto* card = qobject_cast<QFrame*>(widget); card && meetingCards_.contains(card))
            return card;
        widget = widget->parentWidget();
    }
    return nullptr;
}

QFrame* MeetingControllerPane::meetingCardAt(const QPoint& globalPos) const
{
    return meetingCardFor(QApplication::widgetAt(globalPos));
}

void MeetingControllerPane::setCardFlag(QFrame* card, const char* name, bool on)
{
    if (!card || card->property(name).toBool() == on)
        return;

    card->setProperty(name, on);
    card->style()->unpolish(card);
    card->style()->polish(card);
    card->update();
}

void MeetingControllerPane::setDropHover(QFrame* card)
{
    if (dropHoverCard_ == card)
        return;

    setCardFlag(dropHoverCard_, "dropHover", false);
    dropHoverCard_ = card;
    setCardFlag(dropHoverCard_, "dropHover", true);
}

void MeetingControllerPane::clearDrag()
{
    if (QWidget* grabbed = QWidget::mouseGrabber())
    {
        if (meetingCardFor(grabbed))
            grabbed->releaseMouse();
    }
    if (dragCursorSet_)
    {
        QApplication::restoreOverrideCursor();
        dragCursorSet_ = false;
    }
    dragSourceKey_.clear();
    dragMoved_ = false;
    setDropHover(nullptr);
}

bool MeetingControllerPane::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Resize && meetingsArea_ && watched == meetingsArea_->viewport())
        fitMeetingCards();
    const bool entered = event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter;
    const bool left = event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave;
    auto* mouse = dynamic_cast<QMouseEvent*>(event);
    if (!entered && !left && !mouse)
        return QWidget::eventFilter(watched, event);

    auto* widget = qobject_cast<QWidget*>(watched);
    QFrame* card = widget ? meetingCardFor(widget) : nullptr;
    if (!card)
        return QWidget::eventFilter(watched, event);

    if (entered)
    {
        setCardFlag(card, "hovered", true);
        return QWidget::eventFilter(watched, event);
    }
    if (left)
    {
        setCardFlag(card, "hovered", card->rect().contains(card->mapFromGlobal(QCursor::pos())));
        return QWidget::eventFilter(watched, event);
    }

    if (!mouse || qobject_cast<QPushButton*>(widget))
        return QWidget::eventFilter(watched, event);

    const QString key = card->property("meetingKey").toString();
    const bool onTitle = widget->objectName() == "meetingTitle";
    if (event->type() == QEvent::MouseButtonDblClick && mouse->button() == Qt::LeftButton)
    {
        if (!onTitle && !meetingBusy_ && !dragMoved_)
            activateMeeting(key);
        return onTitle ? QWidget::eventFilter(watched, event) : true;
    }

    if (meetingBusy_)
        return QWidget::eventFilter(watched, event);

    if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton)
    {
        if (onTitle && widget->hasFocus())
            return QWidget::eventFilter(watched, event);

        dragSourceKey_ = key;
        dragPressGlobal_ = mouse->globalPosition().toPoint();
        dragMoved_ = false;
        return onTitle ? true : QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::MouseMove
        && !dragSourceKey_.isEmpty()
        && (mouse->buttons() & Qt::LeftButton))
    {
        if ((mouse->globalPosition().toPoint() - dragPressGlobal_).manhattanLength()
            >= QApplication::startDragDistance())
        {
            if (!dragMoved_)
            {
                dragMoved_ = true;
                card->grabMouse();
                QApplication::setOverrideCursor(Qt::ClosedHandCursor);
                dragCursorSet_ = true;
            }
            setDropHover(meetingCardAt(mouse->globalPosition().toPoint()));
        }
        if (dragMoved_)
            return true;
    }

    if (event->type() == QEvent::MouseButtonRelease
        && mouse->button() == Qt::LeftButton
        && dragSourceKey_ == key)
    {
        const bool moved = dragMoved_;
        const QString from = dragSourceKey_;
        QFrame* target = meetingCardAt(mouse->globalPosition().toPoint());
        const QString to = target ? target->property("meetingKey").toString() : QString();
        clearDrag();
        if (moved)
        {
            QTimer::singleShot(0, this, [this, from, to] { moveMeeting(from, to); });
            return true;
        }
        if (onTitle)
            widget->setFocus(Qt::MouseFocusReason);
    }

    return QWidget::eventFilter(watched, event);
}

void MeetingControllerPane::setMeetingBusy(bool busy, const QString& statusMessage)
{
    meetingBusy_ = busy;

    if (!statusMessage.isEmpty())
        setStatusMessage(statusMessage);

    dashboardPickUpButton_->setEnabled(!busy);
    refreshMeetingCards();
}

void MeetingControllerPane::moveMeeting(const QString& fromKey, const QString& toKey)
{
    if (fromKey.isEmpty() || toKey.isEmpty() || fromKey == toKey)
        return;

    int from = -1;
    int to = -1;
    for (int index = 0; index < cardOrder_.size(); ++index)
    {
        const QString key = meetings_.at(cardOrder_.at(index)).key();
        if (key == fromKey)
            from = index;
        if (key == toKey)
            to = index;
    }
    if (from < 0 || to < 0 || from == to)
        return;

    clearDrag();
    cardOrder_.move(from, to);
    meetingCards_.move(from, to);
    layoutMeetingCards();
    appendLog(QString("Moved \"%1\" in the card list.").arg(meetings_.at(cardOrder_.at(to)).title));
}

void MeetingControllerPane::removeMeeting(const QString& key)
{
    for (int index = 0; index < meetings_.size(); ++index)
    {
        if (meetings_.at(index).key() != key)
            continue;

        const QString label = meetings_.at(index).meetingId.isEmpty()
            ? meetings_.at(index).title
            : QString("meeting %1").arg(meetings_.at(index).meetingId);
        appendLog(QString("Removed %1 from managed meetings.").arg(label));

        meetings_.removeAt(index);
        cardOrder_.removeOne(index);
        for (int& row : cardOrder_) {
            if (row > index)
                --row;
        }
        refreshMeetingCards();
        return;
    }
}

void MeetingControllerPane::activateMeeting(const QString& key)
{
    const ManagedMeeting* meeting = findMeeting(key);
    if (!meeting || meetingBusy_)
        return;

    appendLog(QString("Switching Chrome to \"%1\".").arg(meeting->title));
    setMeetingBusy(true, "Switching Chrome to the meeting tab...");
    emit activationRequested(
        meeting->handle(), meeting->tabRuntimeId, meeting->automationName(), key);
}

void MeetingControllerPane::readMicrophone(const QString& key)
{
    const ManagedMeeting* meeting = findMeeting(key);
    if (!meeting || meetingBusy_)
        return;

    appendLog(QString("Reading the microphone control of \"%1\".").arg(meeting->title));
    setMeetingBusy(true, "Reading Zoom's microphone control...");
    emit microphoneReadRequested(
        meeting->handle(), meeting->tabRuntimeId, meeting->automationName(), key);
}

void MeetingControllerPane::setMicrophone(const QString& key, bool muted)
{
    const ManagedMeeting* meeting = findMeeting(key);
    if (!meeting || meetingBusy_)
        return;

    appendLog(QString("%1 \"%2\".")
                  .arg(muted ? "Muting" : "Unmuting", meeting->title));
    setMeetingBusy(true, muted ? "Muting the meeting..." : "Unmuting the meeting...");
    emit microphoneSetRequested(
        meeting->handle(), meeting->tabRuntimeId, meeting->automationName(), muted, key);
}

void MeetingControllerPane::readSound(const QString& key)
{
    const ManagedMeeting* meeting = findMeeting(key);
    if (!meeting || meetingBusy_)
        return;
    appendLog(QString("Reading computer audio for \"%1\".").arg(meeting->title));
    setMeetingBusy(true, "Reading Zoom's sound and microphone states...");
    emit audioReadRequested(meeting->handle(), meeting->tabRuntimeId, meeting->automationName(), key);
}

void MeetingControllerPane::setSound(const QString& key, bool on)
{
    const ManagedMeeting* meeting = findMeeting(key);
    if (!meeting || meetingBusy_)
        return;
    appendLog(QString("Turning sound %1 for \"%2\" by %3 computer audio.")
                  .arg(on ? "on" : "off", meeting->title, on ? "joining" : "leaving"));
    setMeetingBusy(true, on ? "Joining computer audio and verifying sound..."
                           : "Leaving computer audio and verifying sound...");
    emit audioSetRequested(meeting->handle(), meeting->tabRuntimeId, meeting->automationName(), on, key);
}

void MeetingControllerPane::onMeetingActivated(const QJsonObject& result)
{
    const QString key = result.value("requestKey").toString();

    if (!result.value("ok").toBool())
    {
        const QString error = result.value("error").toString();
        appendLog("Could not switch to the meeting tab: " + error);
        setMeetingBusy(false, "Could not switch to the meeting tab");
        QMessageBox::warning(this, "Could not switch to the meeting tab", error);
        return;
    }

    if (ManagedMeeting* meeting = findMeeting(key))
    {
        // The runtime ID changes when Chrome rebuilds the tab strip.
        meeting->tabRuntimeId = result.value("tabRuntimeId").toString();
        const QString chromeName = result.value("tabName").toString();
        if (!chromeName.isEmpty())
        {
            if (meeting->title.isEmpty() || meeting->title == meeting->chromeTabName)
                meeting->title = chromeName;
            meeting->chromeTabName = chromeName;
        }
        appendLog(QString("Switched Chrome to \"%1\".").arg(meeting->title));
    }

    setMeetingBusy(false, "Switched Chrome to the meeting tab");
}

void MeetingControllerPane::applyMicrophoneResult(const QJsonObject& result, const QString& action)
{
    const QString key = result.value("requestKey").toString();
    ManagedMeeting* meeting = findMeeting(key);

    if (!result.value("ok").toBool())
    {
        const QString error = result.value("error").toString();
        appendLog(action + " failed: " + error);

        if (meeting)
            meeting->applyObservation(result);

        setMeetingBusy(false, action + " failed");
        QMessageBox::warning(this, action + " failed", error);
        return;
    }

    const QString state = result.value("state").toString();

    if (meeting)
    {
        meeting->applyObservation(result);
    }

    const QString title = meeting ? meeting->title : QString("meeting");
    const QString verb = result.value("changed").toBool()
        ? QString("is now")
        : QString("was already");

    appendLog(result.contains("changed")
        ? QString("\"%1\" %2 %3 (control reads \"%4\").")
              .arg(title, verb, state, result.value("controlName").toString())
        : QString("\"%1\" microphone reads %2 (control reads \"%3\").")
              .arg(title, state, result.value("controlName").toString()));

    setMeetingBusy(false, QString("%1 · %2").arg(title, state));
}

void MeetingControllerPane::onMicrophoneRead(const QJsonObject& result)
{
    applyMicrophoneResult(result, "Reading the microphone");
}

void MeetingControllerPane::onMicrophoneSet(const QJsonObject& result)
{
    applyMicrophoneResult(
        result, result.value("requestedMuted").toBool() ? "Muting" : "Unmuting");
}

void MeetingControllerPane::applyAudioResult(const QJsonObject& result, const QString& action)
{
    ManagedMeeting* meeting = findMeeting(result.value("requestKey").toString());
    const QString audioState = result.value("audioState").toString();
    const bool verified = result.value("verified").toBool()
        && (audioState == "connected" || audioState == "disconnected");
    const QString sound = audioState == "connected" ? "on" : "off";
    QJsonObject observation = result;
    observation.insert("ok", verified);
    if (meeting)
        meeting->applyObservation(observation);

    if (!result.value("ok").toBool() || !verified)
    {
        const QString error = result.value("error").toString("Zoom's sound state could not be verified.");
        const QString title = meeting ? meeting->title : QString("Meeting");
        appendLog(QString("\"%1\", %2 failed: %3").arg(title, action, error));
        const QString status = verified ? "Sound " + sound : QString("sound state unread");
        setMeetingBusy(false, action + " failed · " + status);
        QMessageBox::warning(this, action + " failed", error);
        return;
    }

    const QString title = meeting ? meeting->title : QString("meeting");
    appendLog(QString("\"%1\" sound is %2 (verified from \"%3\"); microphone is %4.")
                  .arg(title, sound, result.value("audioControlName").toString(), result.value("state").toString()));
    setMeetingBusy(false,
        QString("%1 · Sound %2 · microphone state refreshed")
            .arg(title, sound));
}

void MeetingControllerPane::onAudioConnectionRead(const QJsonObject& result)
{
    applyAudioResult(result, "Reading sound");
}

void MeetingControllerPane::onAudioConnectionSet(const QJsonObject& result)
{
    applyAudioResult(result, result.value("requestedConnected").toBool()
        ? "Turning sound on" : "Turning sound off");
}

void MeetingControllerPane::requestRefreshConnections()
{
    startMeetingBatch(MeetingBatch::Operation::Refresh);
}

void MeetingControllerPane::requestSoundOffAll()
{
    startMeetingBatch(MeetingBatch::Operation::SoundOff);
}

void MeetingControllerPane::requestMuteAll()
{
    startMeetingBatch(MeetingBatch::Operation::Mute);
}

void MeetingControllerPane::startMeetingBatch(MeetingBatch::Operation operation)
{
    if (meetings_.isEmpty() || meetingBusy_ || pickupInProgress_)
        return;

    batchOperation_ = operation;
    batchChanged_ = 0;
    batchAlready_ = 0;
    batchFailures_.clear();
    const QString action = operation == MeetingBatch::Operation::Mute ? "Mute All"
        : operation == MeetingBatch::Operation::SoundOff ? "Sound Off All" : "Connection check";
    const QString message = QString("%1 · %2 meeting(s)").arg(action).arg(meetings_.size());
    appendLog(message);
    setMeetingBusy(true, message);
    QJsonArray targets;
    for (int row : std::as_const(cardOrder_))
        targets.append(meetings_.at(row).automationTarget());
    batch_->start(operation, targets);
}

void MeetingControllerPane::onBatchMeetingFinished(const QJsonObject& result)
{
    const QString key = result.value("requestKey").toString();
    ManagedMeeting* meeting = findMeeting(key);
    const QString title = meeting ? meeting->title : QStringLiteral("Meeting");
    const QString audio = result.value("audioState").toString();
    bool verified = result.value("ok").toBool() && result.value("verified").toBool();
    QString state;
    switch (batchOperation_)
    {
    case MeetingBatch::Operation::Mute:
        verified = verified && result.value("state").toString() == "muted";
        state = "muted";
        break;
    case MeetingBatch::Operation::SoundOff:
        verified = verified && audio == "disconnected";
        state = "sound off";
        break;
    case MeetingBatch::Operation::Refresh:
        verified = verified && (audio == "connected" || audio == "disconnected");
        state = audio == "connected" ? "computer audio connected" : "ready to join computer audio";
        break;
    }

    if (meeting)
    {
        QJsonObject observation = result;
        observation.insert("ok", verified);
        meeting->applyObservation(observation);
        refreshBatchCard(key, *meeting);
    }
    if (verified)
    {
        const bool changed = result.value("changed").toBool();
        if (changed)
            ++batchChanged_;
        else
            ++batchAlready_;
        if (!changed && batchOperation_ != MeetingBatch::Operation::Refresh)
            state.prepend("already ");
        appendLog(title + " · " + state);
    }
    else
    {
        const QString failure = title + " · " + result.value("error").toString("The state could not be verified.");
        batchFailures_.append(failure);
        appendLog(failure);
    }
}

void MeetingControllerPane::onBatchFinished(int skipped)
{
    batchRow_->hide();
    batchProgress_->hide();
    cancelRemaining_->hide();
    QString summary;
    switch (batchOperation_)
    {
    case MeetingBatch::Operation::Mute:
        summary = QString("Mute All · %1 muted, %2 already muted").arg(batchChanged_).arg(batchAlready_);
        break;
    case MeetingBatch::Operation::SoundOff:
        summary = QString("Sound Off All · %1 turned off, %2 already off").arg(batchChanged_).arg(batchAlready_);
        break;
    case MeetingBatch::Operation::Refresh:
        summary = QString("Connection check · %1 verified · %2 active")
            .arg(batchChanged_ + batchAlready_).arg(meetingCountValue_->text());
        break;
    }
    summary += QString(", %1 failed").arg(batchFailures_.size());
    if (skipped > 0)
        summary += QString(", %1 skipped").arg(skipped);
    appendLog(summary);
    setMeetingBusy(false, summary);
    if (!batchFailures_.isEmpty())
        QMessageBox::warning(this, "Some meetings could not be verified", batchFailures_.join("\n\n"));
}

void MeetingControllerPane::setStatusMessage(const QString& message)
{
    statusMessage_ = message;
    emit statusMessageChanged(message);
    appendLog(message);
}

void MeetingControllerPane::appendLog(const QString& message)
{
    if (!liveDiagnostics_ || message.isEmpty() || message == lastLog_)
        return;

    lastLog_ = message;
    const QString timestamp =
        QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    liveDiagnostics_->appendPlainText(QString("[%1] %2").arg(timestamp, message));
}

void MeetingControllerPane::setHeaderPalette(const QPalette& palette)
{
    setStyleSheet(caption_theme::styleSheet(QStringLiteral(":/controller.qss"), palette));
}
