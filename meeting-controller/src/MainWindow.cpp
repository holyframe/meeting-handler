#include "MainWindow.h"
#include "UiaWorker.h"

#include <algorithm>

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QMetaObject>

namespace
{
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

QPushButton* makeNavButton(const QString& text, QWidget* parent)
{
    auto* button = new QPushButton(text, parent);
    button->setObjectName("pageNavButton");
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumHeight(36);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return button;
}

QFrame* makeMetricCard(
    QWidget* parent,
    const QString& symbol,
    const QString& heading,
    const QString& value,
    const QString& detail,
    const QString& accent,
    QLabel** valueOutput,
    QLabel** detailOutput)
{
    auto* card = makePanel(parent, "metricCard");
    auto* row = new QHBoxLayout(card);
    row->setContentsMargins(16, 14, 16, 14);
    row->setSpacing(12);

    auto* iconTile = new QLabel(symbol, card);
    iconTile->setObjectName("metricIcon");
    iconTile->setAlignment(Qt::AlignCenter);
    iconTile->setFixedSize(48, 48);
    iconTile->setStyleSheet(QString(
        "QLabel#metricIcon { color:white; background:%1; border-radius:11px; "
        "font-size:20pt; font-weight:700; }").arg(accent));

    auto* textColumn = new QVBoxLayout();
    textColumn->setSpacing(3);

    auto* headingLabel = new QLabel(heading, card);
    headingLabel->setObjectName("metricHeading");

    auto* valueLabel = new QLabel(value, card);
    valueLabel->setObjectName("metricValue");

    auto* detailLabel = makeMutedLabel(detail, card);
    detailLabel->setObjectName("metricDetail");

    textColumn->addWidget(headingLabel);
    textColumn->addWidget(valueLabel);
    textColumn->addWidget(detailLabel);
    textColumn->addStretch();

    row->addWidget(iconTile, 0, Qt::AlignVCenter);
    row->addLayout(textColumn, 1);

    if (valueOutput)
        *valueOutput = valueLabel;
    if (detailOutput)
        *detailOutput = detailLabel;

    return card;
}

}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("Meeting Controller");
    setMinimumSize(1120, 720);
    resize(minimumSize());

    auto* root = new QWidget(this);
    root->setObjectName("appRoot");
    auto* shell = new QVBoxLayout(root);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);

    shell->addWidget(createPageBar());

    pageStack_ = new QStackedWidget(root);
    pageStack_->setObjectName("pageStack");

    dashboardPage_ = createDashboardPage();
    diagnosticsPage_ = createDiagnosticsPage();
    settingsPage_ = createSettingsPage();
    aboutPage_ = createAboutPage();

    pageStack_->addWidget(dashboardPage_);
    pageStack_->addWidget(diagnosticsPage_);
    pageStack_->addWidget(settingsPage_);
    pageStack_->addWidget(aboutPage_);

    shell->addWidget(pageStack_, 1);
    setCentralWidget(root);

    connect(dashboardNavButton_, &QPushButton::clicked,
            this, [this] { navigateTo(0); });
    connect(diagnosticsNavButton_, &QPushButton::clicked,
            this, [this] { navigateTo(1); });
    connect(settingsNavButton_, &QPushButton::clicked,
            this, [this] { navigateTo(2); });
    connect(aboutNavButton_, &QPushButton::clicked,
            this, [this] { navigateTo(3); });

    connect(dashboardPickUpButton_, &QPushButton::clicked,
            this, &MainWindow::requestMeetingTabImport);

    connect(inspectButton_, &QPushButton::clicked,
            this, &MainWindow::inspectSelectedWindow);

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

    connect(this, &MainWindow::scanRequested,
            worker_, &UiaWorker::scanChromeWindows, Qt::QueuedConnection);
    connect(this, &MainWindow::inspectRequested,
            worker_, &UiaWorker::inspectWindow, Qt::QueuedConnection);
    connect(this, &MainWindow::pickupRequested,
            worker_, &UiaWorker::listChromeTabs, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::chromeTabsListed,
            this, &MainWindow::onChromeTabsListed, Qt::QueuedConnection);
    connect(this, &MainWindow::activationRequested,
            worker_, &UiaWorker::activateMeeting, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::meetingActivated,
            this, &MainWindow::onMeetingActivated, Qt::QueuedConnection);
    connect(this, &MainWindow::microphoneReadRequested,
            worker_, &UiaWorker::readMicrophone, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::microphoneRead,
            this, &MainWindow::onMicrophoneRead, Qt::QueuedConnection);
    connect(this, &MainWindow::microphoneSetRequested,
            worker_, &UiaWorker::setMicrophoneMuted, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::microphoneSet,
            this, &MainWindow::onMicrophoneSet, Qt::QueuedConnection);
    connect(this, &MainWindow::audioReadRequested,
            worker_, &UiaWorker::readAudioConnection, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::audioConnectionRead,
            this, &MainWindow::onAudioConnectionRead, Qt::QueuedConnection);
    connect(this, &MainWindow::audioSetRequested,
            worker_, &UiaWorker::setAudioConnected, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::audioConnectionSet,
            this, &MainWindow::onAudioConnectionSet, Qt::QueuedConnection);
    batch_ = new MeetingBatch(this);
    connect(batch_, &MeetingBatch::meetingStarted, this, [this](int number, int total, const QString& key) {
        const auto* meeting = findMeeting(key);
        const QString progress = QString("Checking meeting %1 of %2").arg(number).arg(total);
        batchProgress_->setText(progress);
        batchProgress_->setToolTip(meeting ? meeting->title : QString());
        batchProgress_->show();
        cancelRemaining_->setEnabled(number < total);
        cancelRemaining_->show();
        statusBar()->showMessage(meeting ? progress + " · " + meeting->title : progress);
    });
    connect(batch_, &MeetingBatch::meetingFinished, this, &MainWindow::onBatchMeetingFinished);
    connect(batch_, &MeetingBatch::finished, this, &MainWindow::onBatchFinished);
    connect(batch_, &MeetingBatch::initialized, this, &MainWindow::onInitialized);
    connect(batch_, &MeetingBatch::diagnostic, this, &MainWindow::appendLog);
    connect(cancelRemaining_, &QPushButton::clicked, this, [this] {
        batch_->cancelRemaining();
        cancelRemaining_->setEnabled(false);
        batchProgress_->setText("Finishing the current meeting");
        appendLog("Cancelled the remaining meetings in this batch.");
    });
    connect(this, &MainWindow::prepareImportedMeetingsRequested,
            worker_, &UiaWorker::prepareImportedMeetings, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::importedMeetingsPrepared,
            this, &MainWindow::onImportedMeetingsPrepared, Qt::QueuedConnection);

    connect(dashboardMuteAllButton_, &QPushButton::clicked,
            this, &MainWindow::requestMuteAll);
    connect(dashboardSoundOffButton_, &QPushButton::clicked,
            this, &MainWindow::requestSoundOffAll);
    connect(uiaRefreshButton_, &QPushButton::clicked,
            this, &MainWindow::requestRefreshConnections);
    connect(worker_, &UiaWorker::initialized,
            this, &MainWindow::onInitialized, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::chromeWindowsFound,
            this, &MainWindow::onChromeWindowsFound, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::inspectionFinished,
            this, &MainWindow::onInspectionFinished, Qt::QueuedConnection);
    connect(worker_, &UiaWorker::diagnostic,
            this, &MainWindow::appendLog, Qt::QueuedConnection);
    connect(workerThread_, &QThread::finished,
            worker_, &QObject::deleteLater);

    applyDarkProfessionalTheme();
    navigateTo(0);
    statusBar()->showMessage(
        "Ready · Pick up meeting tabs to read microphone and sound states");
    workerThread_->start();
}

MainWindow::~MainWindow()
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

QWidget* MainWindow::createPageBar()
{
    auto* bar = new QFrame(this);
    bar->setObjectName("pageBar");

    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(20, 10, 20, 10);
    layout->setSpacing(8);

    dashboardNavButton_ = makeNavButton("Dashboard", bar);
    diagnosticsNavButton_ = makeNavButton("Live Diagnostics", bar);
    settingsNavButton_ = makeNavButton("Settings", bar);
    aboutNavButton_ = makeNavButton("About", bar);

    layout->addWidget(dashboardNavButton_);
    layout->addWidget(diagnosticsNavButton_);
    layout->addWidget(settingsNavButton_);
    layout->addWidget(aboutNavButton_);
    layout->addStretch();
    return bar;
}

QWidget* MainWindow::createDashboardPage()
{
    auto* page = new QWidget(this);
    page->setObjectName("pageRoot");

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 22, 28, 20);
    layout->setSpacing(18);

    auto* metrics = new QGridLayout();
    metrics->setHorizontalSpacing(14);
    metrics->setVerticalSpacing(14);

    dashboardMuteAllButton_ = new QPushButton("◉  Mute All Meetings", page);
    dashboardMuteAllButton_->setObjectName("dangerButton");
    dashboardMuteAllButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    dashboardMuteAllButton_->setEnabled(false);
    dashboardMuteAllButton_->setToolTip("Pick up a meeting tab first.");
    metrics->addWidget(dashboardMuteAllButton_, 0, 0);

    metrics->addWidget(makeMetricCard(
        page, "♟", "Meeting Counts", "0/0", "Active / total",
        "#1765C5", &meetingCountValue_, &meetingCountDetail_), 0, 1);

    dashboardSoundOffButton_ = new QPushButton("Sound Off All", page);
    dashboardSoundOffButton_->setObjectName("soundOffAllButton");
    dashboardSoundOffButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    dashboardSoundOffButton_->setMinimumWidth(0);
    dashboardSoundOffButton_->setEnabled(false);
    dashboardSoundOffButton_->setToolTip("Pick up a meeting tab first.");
    metrics->addWidget(dashboardSoundOffButton_, 0, 2);

    auto* uiaCard = makeMetricCard(
        page, "▣", "UIA Connection", "Idle", "Starts on first inspection",
        "#1765C5", &uiaValue_, &uiaDetail_);
    uiaRefreshButton_ = new QPushButton("Refresh", uiaCard);
    uiaRefreshButton_->setObjectName("uiaRefreshButton");
    uiaRefreshButton_->setToolTip(
        "Read every meeting's computer-audio connection and update which cards are active. "
        "Chrome switches to each meeting tab to read it.");
    uiaRefreshButton_->setEnabled(false);
    if (auto* uiaRow = qobject_cast<QHBoxLayout*>(uiaCard->layout()))
        uiaRow->addWidget(uiaRefreshButton_, 0, Qt::AlignVCenter);
    metrics->addWidget(uiaCard, 0, 3);

    dashboardPickUpButton_ = new QPushButton("＋  Pick Up Meeting Tabs", page);
    dashboardPickUpButton_->setObjectName("primaryButton");
    dashboardPickUpButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    metrics->addWidget(dashboardPickUpButton_, 0, 4);

    for (int column = 0; column < 5; ++column)
        metrics->setColumnStretch(column, 1);
    layout->addLayout(metrics);

    auto* meetingHeader = new QHBoxLayout();
    auto* meetingTitle = new QLabel("♟  Managed Meetings", page);
    meetingTitle->setObjectName("sectionTitle");
    meetingCountLabel_ = new QLabel("0", page);
    meetingCountLabel_->setObjectName("countPill");
    meetingHeader->addWidget(meetingTitle);
    meetingHeader->addWidget(meetingCountLabel_);
    meetingHeader->addStretch();
    batchProgress_ = new QLabel(page);
    batchProgress_->setObjectName("batchProgress");
    batchProgress_->hide();
    meetingHeader->addWidget(batchProgress_);
    cancelRemaining_ = new QPushButton("Cancel remaining", page);
    cancelRemaining_->setObjectName("secondaryButton");
    cancelRemaining_->setToolTip("Finish the current meeting and skip the remaining meetings.");
    cancelRemaining_->hide();
    meetingHeader->addWidget(cancelRemaining_);
    layout->addLayout(meetingHeader);

    auto* meetingsArea = new QScrollArea(page);
    meetingsArea_ = meetingsArea;
    meetingsArea->setObjectName("meetingsScrollArea");
    meetingsArea->setWidgetResizable(true);
    meetingsArea->setFrameShape(QFrame::NoFrame);

    auto* meetingsContent = new QWidget(meetingsArea);
    meetingsContent->setObjectName("meetingsContent");
    meetingsContent->setObjectName("meetingsContent");
    auto* meetingsLayout = new QGridLayout(meetingsContent);
    meetingsLayout->setContentsMargins(0, 2, 0, 8);
    meetingsLayout->setSpacing(14);
    meetingsLayout->setAlignment(Qt::AlignTop);
    for (int column = 0; column < 3; ++column)
        meetingsLayout->setColumnStretch(column, 1);
    meetingsLayout_ = meetingsLayout;

    auto* emptyCard = makePanel(meetingsContent, "emptyMeetingsCard");
    meetingEmptyCard_ = emptyCard;
    auto* emptyLayout = new QVBoxLayout(emptyCard);
    emptyLayout->setContentsMargins(24, 34, 24, 34);
    emptyLayout->setSpacing(10);
    emptyLayout->setAlignment(Qt::AlignCenter);

    auto* emptyIcon = new QLabel("▣", emptyCard);
    emptyIcon->setObjectName("emptyIcon");
    emptyIcon->setAlignment(Qt::AlignCenter);
    auto* emptyTitle = new QLabel("No meetings picked up", emptyCard);
    emptyTitle->setObjectName("emptyTitle");
    auto* emptyDescription = makeMutedLabel(
        "Choose Pick Up Meeting Tabs, then select one or more Chrome tabs to import.",
        emptyCard);
    emptyDescription->setAlignment(Qt::AlignCenter);
    emptyDescription->setMaximumWidth(520);

    emptyLayout->addWidget(emptyIcon);
    emptyLayout->addWidget(emptyTitle);
    emptyLayout->addWidget(emptyDescription);
    meetingsLayout->addWidget(emptyCard, 0, 0, 1, 3);

    meetingsArea->setWidget(meetingsContent);
    layout->addWidget(meetingsArea, 1);

    auto* footerHint = makeMutedLabel(
        "Controls read Zoom's state before and after each change. Sound Off leaves computer audio, including your microphone.",
        page);
    layout->addWidget(footerHint);

    return page;
}

QWidget* MainWindow::createDiagnosticsPage()
{
    auto* page = new QWidget(this);
    page->setObjectName("pageRoot");

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 22, 28, 20);
    layout->setSpacing(16);

    auto* title = new QLabel("Live Diagnostics", page);
    title->setObjectName("pageTitle");
    auto* subtitle = makeMutedLabel(
        "Inspect existing Chrome windows and review their Windows UI Automation accessibility tree.",
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

    auto* controls = new QHBoxLayout();
    auto* browserLabel = new QLabel("Chrome window", browserPanel);
    browserLabel->setObjectName("mutedText");

    chromeWindowCombo_ = new QComboBox(browserPanel);
    chromeWindowCombo_->setObjectName("chromeWindowCombo");
    chromeWindowCombo_->setMinimumWidth(340);
    chromeWindowCombo_->addItem("Click Refresh to list Chrome windows", QString());

    auto* refreshButton = new QPushButton("⟳  Refresh", browserPanel);
    refreshButton->setObjectName("secondaryButton");
    refreshButton->setMinimumHeight(40);

    inspectButton_ = new QPushButton("Inspect Selected Window", browserPanel);
    inspectButton_->setObjectName("secondaryButton");
    inspectButton_->setMinimumHeight(40);
    inspectButton_->setEnabled(false);

    controls->addWidget(browserLabel);
    controls->addWidget(chromeWindowCombo_, 1);
    controls->addWidget(refreshButton);
    controls->addWidget(inspectButton_);
    browserLayout->addLayout(controls);
    layout->addWidget(browserPanel);

    diagnosticsTabs_ = new QTabWidget(page);
    diagnosticsTabs_->setObjectName("diagnosticsTabs");

    liveDiagnostics_ = new QPlainTextEdit(diagnosticsTabs_);
    liveDiagnostics_->setObjectName("diagnosticsText");
    liveDiagnostics_->setReadOnly(true);
    liveDiagnostics_->setPlaceholderText(
        "Timestamped UI Automation events, scans, warnings, and errors will appear here.");

    accessibilityTree_ = new QPlainTextEdit(diagnosticsTabs_);
    accessibilityTree_->setObjectName("diagnosticsText");
    accessibilityTree_->setReadOnly(true);
    accessibilityTree_->setPlaceholderText(
        "Inspect a Chrome window to view its UI Automation accessibility tree.");

    diagnosticsTabs_->addTab(liveDiagnostics_, "Live Activity");
    diagnosticsTabs_->addTab(accessibilityTree_, "Accessibility Tree");
    layout->addWidget(diagnosticsTabs_, 1);

    auto* note = makeMutedLabel(
        "Read-only diagnostics. This build does not click page controls or mute/unmute Zoom.",
        page);
    layout->addWidget(note);

    connect(refreshButton, &QPushButton::clicked,
            this, &MainWindow::refreshChromeWindows);

    return page;
}

QWidget* MainWindow::createSettingsPage()
{
    auto* page = new QWidget(this);
    page->setObjectName("pageRoot");
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 22, 28, 20);
    layout->setSpacing(16);

    auto* title = new QLabel("Settings", page);
    title->setObjectName("pageTitle");
    auto* subtitle = makeMutedLabel(
        "Application behavior and safety configuration.", page);
    subtitle->setObjectName("pageSubtitle");
    layout->addWidget(title);
    layout->addWidget(subtitle);

    auto* safetyPanel = makePanel(page, "panel");
    auto* safetyLayout = new QVBoxLayout(safetyPanel);
    safetyLayout->setContentsMargins(18, 16, 18, 16);
    safetyLayout->setSpacing(8);
    auto* safetyHeading = new QLabel("MICROPHONE SAFETY", safetyPanel);
    safetyHeading->setObjectName("panelHeading");
    auto* safetyText = new QLabel(
        "Microphone and sound controls read Zoom's state before acting and verify it "
        "afterwards. Sound Off leaves computer audio and disconnects the microphone too. "
        "Sound On rejoins computer audio and reads the resulting microphone state.", safetyPanel);
    safetyText->setObjectName("settingsBody");
    safetyText->setWordWrap(true);
    safetyLayout->addWidget(safetyHeading);
    safetyLayout->addWidget(safetyText);
    layout->addWidget(safetyPanel);

    auto* browserPanel = makePanel(page, "panel");
    auto* browserLayout = new QVBoxLayout(browserPanel);
    browserLayout->setContentsMargins(18, 16, 18, 16);
    browserLayout->setSpacing(8);
    auto* browserHeading = new QLabel("BROWSER INTEGRATION", browserPanel);
    browserHeading->setObjectName("panelHeading");
    auto* browserText = new QLabel(
        "Uses Windows UI Automation to inspect existing Chrome windows. No Chrome "
        "extension, remote-debugging port, dedicated Chrome profile, or browser "
        "launch changes are required.", browserPanel);
    browserText->setObjectName("settingsBody");
    browserText->setWordWrap(true);
    browserLayout->addWidget(browserHeading);
    browserLayout->addWidget(browserText);
    layout->addWidget(browserPanel);
    layout->addStretch();

    return page;
}

QWidget* MainWindow::createAboutPage()
{
    auto* page = new QWidget(this);
    page->setObjectName("pageRoot");
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 22, 28, 20);
    layout->setSpacing(16);

    auto* title = new QLabel("About Meeting Controller", page);
    title->setObjectName("pageTitle");
    auto* subtitle = makeMutedLabel(
        "A native Windows controller for managing existing Chrome Zoom sessions.",
        page);
    subtitle->setObjectName("pageSubtitle");
    layout->addWidget(title);
    layout->addWidget(subtitle);

    auto* panel = makePanel(page, "panel");
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(20, 18, 20, 18);
    panelLayout->setSpacing(10);

    auto* appName = new QLabel("Meeting Controller", panel);
    appName->setObjectName("sectionTitle");
    auto* version = makeMutedLabel("Version 0.4.0 · Windows UIA feasibility build", panel);
    auto* technology = new QLabel(
        "C++20 · Qt 6 Widgets · Windows UI Automation · CMake", panel);
    technology->setObjectName("settingsBody");
    auto* scope = makeMutedLabel(
        "Current scope: Chrome window discovery, accessibility-tree inspection, and "
        "picking up Chrome tabs you choose, and verified microphone and computer-audio control.", panel);
    panelLayout->addWidget(appName);
    panelLayout->addWidget(version);
    panelLayout->addWidget(technology);
    panelLayout->addWidget(scope);

    layout->addWidget(panel);
    layout->addStretch();
    return page;
}

void MainWindow::navigateTo(int pageIndex)
{
    if (!pageStack_ || pageIndex < 0 || pageIndex >= pageStack_->count())
        return;

    pageStack_->setCurrentIndex(pageIndex);
    dashboardNavButton_->setChecked(pageIndex == 0);
    diagnosticsNavButton_->setChecked(pageIndex == 1);
    settingsNavButton_->setChecked(pageIndex == 2);
    aboutNavButton_->setChecked(pageIndex == 3);

    if (pageIndex == 1)
        statusBar()->showMessage("Live Diagnostics · Read-only UI Automation inspection");
    else if (pageIndex == 0)
        statusBar()->showMessage("Dashboard · Microphone automation disabled pending validation");
    else
        statusBar()->showMessage("Meeting Controller");
}

void MainWindow::onInitialized(bool success, const QString& message)
{
    updateUiaStatus(success, message);
}

void MainWindow::updateUiaStatus(bool ready, const QString& message)
{
    const QString state = ready ? "Ready" : "Unavailable";
    uiaValue_->setText(state);
    uiaDetail_->setText(ready
        ? "Windows UI Automation initialized"
        : "Initialization failed");

    if (batchProgress_->isHidden())
        statusBar()->showMessage(message);
    appendLog(message);
}

void MainWindow::refreshChromeWindows()
{
    if (scanInProgress_)
        return;

    scanInProgress_ = true;
    statusBar()->showMessage("Scanning visible Chrome windows...");
    appendLog("Chrome window scan started.");
    emit scanRequested();
}

void MainWindow::onChromeWindowsFound(const QJsonArray& windows)
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

    statusBar()->showMessage(
        QString("Chrome scan complete · %1 visible window(s) detected").arg(count));
    appendLog(QString("Chrome window scan completed: %1 visible window(s) found.")
                  .arg(count));
}

quintptr MainWindow::selectedWindowHandle() const
{
    bool ok = false;
    const qulonglong value =
        chromeWindowCombo_->currentData().toString().toULongLong(&ok, 16);
    return ok ? static_cast<quintptr>(value) : 0;
}

void MainWindow::inspectSelectedWindow()
{
    const quintptr hwnd = selectedWindowHandle();
    if (!hwnd)
    {
        appendLog("Inspection skipped: no Chrome window is selected.");
        return;
    }

    inspectButton_->setEnabled(false);
    statusBar()->showMessage("Inspecting selected Chrome window through UI Automation...");
    appendLog("UIA inspection requested for HWND 0x"
        + QString::number(static_cast<qulonglong>(hwnd), 16).toUpper());
    emit inspectRequested(hwnd);
}

void MainWindow::onInspectionFinished(const QJsonObject& result)
{
    accessibilityTree_->setPlainText(
        QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Indented)));
    diagnosticsTabs_->setCurrentIndex(1);
    navigateTo(1);

    if (result.value("ok").toBool())
    {
        const int count = result.value("visitedNodes").toInt();
        statusBar()->showMessage(
            QString("UIA inspection complete · %1 nodes visited").arg(count));
        appendLog(QString("UIA inspection completed: %1 nodes visited.").arg(count));
    }
    else
    {
        const QString error = result.value("error").toString();
        statusBar()->showMessage("UIA inspection failed");
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

    if (tab.value("zoomMeeting").toBool())
        parts.append(QString("Zoom meeting %1").arg(tab.value("meetingId").toString()));
    else if (tab.value("addressKnown").toBool())
        parts.append(tab.value("host").toString());
    else
        parts.append("Address not read");

    if (tab.value("selected").toBool())
        parts.append("Shown in this window");

    const QString windowTitle = tab.value("windowTitle").toString();
    if (!windowTitle.isEmpty())
        parts.append(windowTitle);

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
        "Google Chrome browser tabs, grouped by profile. Other Chromium browsers "
        "are not listed. Check one or more tabs to import. A page address is "
        "available only for the tab currently shown in each window.",
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

void MainWindow::requestMeetingTabImport()
{
    if (pickupInProgress_ || meetingBusy_)
        return;

    pickupInProgress_ = true;
    dashboardPickUpButton_->setEnabled(false);
    statusBar()->showMessage("Reading Chrome tabs through UI Automation...");
    appendLog("Chrome tab list requested.");
    emit pickupRequested();
}

void MainWindow::onChromeTabsListed(const QJsonObject& result)
{
    pickupInProgress_ = false;
    dashboardPickUpButton_->setEnabled(true);

    if (!result.value("ok").toBool())
    {
        const QString error = result.value("error").toString();
        statusBar()->showMessage("Could not list Chrome tabs");
        appendLog("Chrome tab list failed: " + error);
        QMessageBox::warning(this, "Could not list Chrome tabs", error);
        return;
    }

    const QJsonArray tabs = result.value("tabs").toArray();
    appendLog(QString("Found %1 Chrome tab(s) across %2 window(s).")
                  .arg(tabs.size())
                  .arg(result.value("windowCount").toInt()));

    const QList<int> selected = askTabsToImport(this, tabs);
    if (selected.isEmpty())
    {
        statusBar()->showMessage("Tab import cancelled");
        appendLog("Tab import cancelled.");
        return;
    }

    int imported = 0;
    int skipped = 0;
    QJsonArray targets;

    for (int index : selected)
    {
        const QJsonObject tab = tabs.at(index).toObject();

        ManagedMeeting meeting;
        meeting.windowHandle = tab.value("windowHandle").toString();
        meeting.processId = tab.value("processId").toInteger();
        meeting.tabRuntimeId = tab.value("tabRuntimeId").toString();
        meeting.chromeTabName = tab.value("tabName").toString();
        meeting.title = meeting.chromeTabName;
        meeting.host = tab.value("host").toString();
        meeting.meetingId = tab.value("meetingId").toString();
        meeting.profileName = tab.value("profileName").toString();

        const bool duplicate = std::any_of(
            meetings_.cbegin(), meetings_.cend(),
            [&meeting](const ManagedMeeting& existing)
            {
                if (existing.key() == meeting.key())
                    return true;
                return !meeting.meetingId.isEmpty()
                    && existing.meetingId == meeting.meetingId;
            });

        if (duplicate)
        {
            ++skipped;
            appendLog(QString("Skipped \"%1\": already managed.").arg(meeting.title));
            continue;
        }

        meetings_.append(meeting);
        ++imported;
        targets.append(QJsonObject{
            {"windowHandle", meeting.windowHandle},
            {"tabRuntimeId", meeting.tabRuntimeId},
            {"tabName", meeting.automationName()},
            {"requestKey", meeting.key()}
        });
        appendLog(QString("Imported \"%1\" from %2.")
                      .arg(meeting.title,
                           meeting.profileName.isEmpty()
                               ? QString("Google Chrome")
                               : meeting.profileName));
    }

    if (imported == 0)
    {
        statusBar()->showMessage(
            QString("Imported 0 tab(s) · %1 already managed").arg(skipped));
        return;
    }

    appendLog(QString("Opening %1 imported meeting(s) and reading the microphone.")
                  .arg(imported));
    setMeetingBusy(true, "Opening the meeting and reading the microphone...");
    emit prepareImportedMeetingsRequested(targets);
}

void MainWindow::onImportedMeetingsPrepared(const QJsonArray& results)
{
    QStringList failures;

    for (const QJsonValue& value : results)
    {
        const QJsonObject result = value.toObject();
        ManagedMeeting* meeting = findMeeting(result.value("requestKey").toString());
        if (!meeting)
            continue;

        updateMeetingStates(*meeting, result);

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

QFrame* MainWindow::createMeetingCard(const ManagedMeeting& meeting)
{
    const QString key = meeting.key();
    const bool isActive = meetingCardIsActive(meeting);

    auto* card = makePanel(
        meetingsLayout_->parentWidget(), isActive ? "meetingCardActive" : "meetingCard");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(11);

    auto* iconTile = new QLabel("▣", card);
    iconTile->setObjectName("meetingIcon");
    iconTile->setAlignment(Qt::AlignCenter);
    iconTile->setFixedSize(38, 38);

    auto* titleColumn = new QVBoxLayout();
    titleColumn->setSpacing(1);

    auto* title = new QLineEdit(
        meeting.title.isEmpty() ? QStringLiteral("Zoom meeting") : meeting.title, card);
    title->setObjectName("meetingTitle");
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
                appendLog(QString("Renamed a meeting card to \"%1\".").arg(name));
            }
        }
    });

    const QString sourceText = meeting.profileName.isEmpty()
        ? QString("Chrome (Window %1)").arg(meeting.windowNumber)
        : meeting.profileName;
    auto* source = makeMutedLabel(sourceText, card);
    source->setAttribute(Qt::WA_TransparentForMouseEvents);

    titleColumn->addWidget(title);
    titleColumn->addWidget(source);

    headerRow->addWidget(iconTile, 0, Qt::AlignTop);
    headerRow->addLayout(titleColumn, 1);
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
        headerRow->addWidget(badge, 0, Qt::AlignTop);
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
    headerRow->addWidget(closeButton, 0, Qt::AlignTop);

    layout->addLayout(headerRow);

    auto* stateRow = new QHBoxLayout();
    stateRow->setSpacing(8);

    auto* microphone = new QLabel(card);
    switch (meeting.microphone)
    {
    case Microphone::Muted:
        microphone->setText("◉ Mic muted");
        microphone->setObjectName("micMuted");
        break;
    case Microphone::Unmuted:
        microphone->setText("◉ Mic on");
        microphone->setObjectName("micUnmuted");
        break;
    case Microphone::AudioNotConnected:
        microphone->setText("◉ Mic disconnected");
        microphone->setObjectName("micUnknown");
        break;
    default:
        microphone->setText("◉ Mic unread");
        microphone->setObjectName("micUnknown");
        break;
    }

    auto* sound = new QLabel(card);
    sound->setObjectName("soundState");
    sound->setProperty("state", meeting.sound == Sound::On ? "connected"
        : meeting.sound == Sound::Off ? "disconnected" : "unknown");
    sound->setText(meeting.sound == Sound::On ? "Sound on"
        : meeting.sound == Sound::Off ? "Sound off" : "Sound unread");
    sound->setToolTip("Shows the last computer-audio connection state read from Zoom. "
                      "It does not measure speaker volume.");
    microphone->setAttribute(Qt::WA_TransparentForMouseEvents);
    sound->setAttribute(Qt::WA_TransparentForMouseEvents);
    stateRow->addWidget(microphone);
    stateRow->addStretch();
    stateRow->addWidget(sound);
    layout->addLayout(stateRow);

    auto* identity = makeMutedLabel(
        meeting.meetingId.isEmpty()
            ? QString("Chrome tab")
            : QString("Zoom Meeting %1").arg(meeting.meetingId),
        card);
    identity->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(identity);

    auto* actionRow = new QHBoxLayout();
    actionRow->setSpacing(8);

    const auto addMicrophoneButton = [this, card, actionRow, key](const QString& label, bool mute)
    {
        auto* button = new QPushButton(label, card);
        button->setObjectName(mute ? "dangerButton" : "secondaryButton");
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        button->setEnabled(!meetingBusy_);
        connect(button, &QPushButton::clicked,
                this, [this, key, mute] { setMicrophone(key, mute); });
        actionRow->addWidget(button, 3);
    };

    if (meeting.microphone == Microphone::Muted)
        addMicrophoneButton("◉  Unmute", false);
    else if (meeting.microphone == Microphone::Unmuted)
        addMicrophoneButton("◉  Mute", true);
    else if (meeting.microphone != Microphone::AudioNotConnected)
    {
        addMicrophoneButton("◉  Mute", true);
        addMicrophoneButton("◉  Unmute", false);
    }

    auto* soundButton = new QPushButton(card);
    soundButton->setObjectName("soundButton");
    soundButton->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    soundButton->setEnabled(!meetingBusy_);
    if (meeting.sound == Sound::Unknown)
    {
        soundButton->setText("Read Sound");
        soundButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
        soundButton->setToolTip("Read Zoom's computer-audio state before choosing Sound On or Off.");
        connect(soundButton, &QPushButton::clicked,
                this, [this, key] { readSound(key); });
    }
    else
    {
        const bool turnOn = meeting.sound == Sound::Off;
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

void MainWindow::refreshMeetingCards()
{
    clearDrag();

    for (QFrame* card : std::as_const(meetingCards_))
    {
        meetingsLayout_->removeWidget(card);
        card->deleteLater();
    }
    meetingCards_.clear();

    meetingEmptyCard_->setVisible(meetings_.isEmpty());

    QStringList windowOrder;
    for (ManagedMeeting& meeting : meetings_)
    {
        if (!windowOrder.contains(meeting.windowHandle))
            windowOrder.append(meeting.windowHandle);

        meeting.windowNumber = windowOrder.indexOf(meeting.windowHandle) + 1;
    }

    for (int index = 0; index < meetings_.size(); ++index)
    {
        QFrame* card = createMeetingCard(meetings_.at(index));
        meetingCards_.append(card);
    }
    layoutMeetingCards();
    updateMeetingCounts();

    const bool canUseMeetings = !meetings_.isEmpty() && !meetingBusy_;
    dashboardMuteAllButton_->setEnabled(canUseMeetings);
    dashboardSoundOffButton_->setEnabled(canUseMeetings);
    uiaRefreshButton_->setEnabled(canUseMeetings);
    uiaRefreshButton_->setToolTip(meetings_.isEmpty()
        ? QString("Pick up a meeting tab first.")
        : QString("Read every meeting's computer-audio connection and update which cards are active. "
                  "Chrome switches to each meeting tab to read it."));

    const QString muteAllTip = meetings_.isEmpty()
        ? QString("Pick up a meeting tab first.")
        : QString("Switches to each meeting tab in turn and mutes it, verifying each result.");
    dashboardMuteAllButton_->setToolTip(muteAllTip);
    dashboardSoundOffButton_->setToolTip(meetings_.isEmpty()
        ? QString("Pick up a meeting tab first.")
        : QString("Leaves computer audio for every meeting. Sound turns off and the microphone disconnects. "
                  "Chrome switches to each meeting tab."));
}

void MainWindow::updateMeetingCounts()
{
    const auto active = std::count_if(meetings_.cbegin(), meetings_.cend(),
        [this](const ManagedMeeting& meeting) { return meetingCardIsActive(meeting); });
    meetingCountValue_->setText(QString("%1/%2").arg(active).arg(meetings_.size()));
    meetingCountDetail_->setText(meetings_.isEmpty() ? "No meetings picked up yet" : "Active / total");
    meetingCountLabel_->setText(QString::number(meetings_.size()));
}

void MainWindow::refreshBatchCard(const QString& previousKey, const ManagedMeeting& meeting)
{
    for (QFrame*& card : meetingCards_)
    {
        if (card->property("meetingKey").toString() != previousKey)
            continue;
        QFrame* replacement = createMeetingCard(meeting);
        delete meetingsLayout_->replaceWidget(card, replacement);
        card->hide();
        card->deleteLater();
        card = replacement;
        replacement->show();
        break;
    }
    updateMeetingCounts();
}

MainWindow::ManagedMeeting* MainWindow::findMeeting(const QString& key)
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

void MainWindow::layoutMeetingCards()
{
    if (!meetingsArea_ || !meetingsLayout_)
        return;
    const int columns = std::clamp((meetingsArea_->viewport()->width() + 14) / 334, 1, 3);
    for (QFrame* card : std::as_const(meetingCards_))
        meetingsLayout_->removeWidget(card);
    for (int column = 0; column < 3; ++column)
        meetingsLayout_->setColumnStretch(column, column < columns ? 1 : 0);
    meetingsLayout_->removeWidget(meetingEmptyCard_);
    meetingsLayout_->addWidget(meetingEmptyCard_, 0, 0, 1, columns);
    for (int index = 0; index < meetingCards_.size(); ++index)
        meetingsLayout_->addWidget(meetingCards_.at(index), index / columns, index % columns);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    QTimer::singleShot(0, this, [this] { layoutMeetingCards(); });
}

QFrame* MainWindow::meetingCardFor(QWidget* widget) const
{
    while (widget)
    {
        if (auto* card = qobject_cast<QFrame*>(widget); card && meetingCards_.contains(card))
            return card;
        widget = widget->parentWidget();
    }
    return nullptr;
}

QFrame* MainWindow::meetingCardAt(const QPoint& globalPos) const
{
    return meetingCardFor(QApplication::widgetAt(globalPos));
}

void MainWindow::setCardFlag(QFrame* card, const char* name, bool on)
{
    if (!card || card->property(name).toBool() == on)
        return;

    card->setProperty(name, on);
    card->style()->unpolish(card);
    card->style()->polish(card);
    card->update();
}

void MainWindow::setDropHover(QFrame* card)
{
    if (dropHoverCard_ == card)
        return;

    setCardFlag(dropHoverCard_, "dropHover", false);
    dropHoverCard_ = card;
    setCardFlag(dropHoverCard_, "dropHover", true);
}

void MainWindow::clearDrag()
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

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    auto* widget = qobject_cast<QWidget*>(watched);
    QFrame* card = widget ? meetingCardFor(widget) : nullptr;
    if (!card)
        return QMainWindow::eventFilter(watched, event);

    const QString key = card->property("meetingKey").toString();
    const bool onButton = qobject_cast<QPushButton*>(widget) != nullptr;
    const bool onTitle = widget->objectName() == "meetingTitle";

    if (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter)
    {
        setCardFlag(card, "hovered", true);
        return QMainWindow::eventFilter(watched, event);
    }
    if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave)
    {
        setCardFlag(card, "hovered", card->rect().contains(card->mapFromGlobal(QCursor::pos())));
        return QMainWindow::eventFilter(watched, event);
    }

    auto* mouse = dynamic_cast<QMouseEvent*>(event);
    if (!mouse || onButton)
        return QMainWindow::eventFilter(watched, event);

    if (event->type() == QEvent::MouseButtonDblClick && mouse->button() == Qt::LeftButton)
    {
        if (!onTitle && !meetingBusy_ && !dragMoved_)
            activateMeeting(key);
        return onTitle ? QMainWindow::eventFilter(watched, event) : true;
    }

    if (meetingBusy_)
        return QMainWindow::eventFilter(watched, event);

    if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton)
    {
        if (onTitle && widget->hasFocus())
            return QMainWindow::eventFilter(watched, event);

        dragSourceKey_ = key;
        dragPressGlobal_ = mouse->globalPosition().toPoint();
        dragMoved_ = false;
        return onTitle ? true : QMainWindow::eventFilter(watched, event);
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

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::setMeetingBusy(bool busy, const QString& statusMessage)
{
    meetingBusy_ = busy;

    if (!statusMessage.isEmpty())
        statusBar()->showMessage(statusMessage);

    dashboardPickUpButton_->setEnabled(!busy);
    refreshMeetingCards();
}

void MainWindow::moveMeeting(const QString& fromKey, const QString& toKey)
{
    if (fromKey.isEmpty() || toKey.isEmpty() || fromKey == toKey)
        return;

    int from = -1;
    int to = -1;
    for (int index = 0; index < meetings_.size(); ++index)
    {
        if (meetings_.at(index).key() == fromKey)
            from = index;
        if (meetings_.at(index).key() == toKey)
            to = index;
    }
    if (from < 0 || to < 0 || from == to)
        return;

    meetings_.move(from, to);
    appendLog(QString("Moved \"%1\" in the meeting list.").arg(meetings_.at(to).title));
    refreshMeetingCards();
}

void MainWindow::removeMeeting(const QString& key)
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
        refreshMeetingCards();
        return;
    }
}

void MainWindow::activateMeeting(const QString& key)
{
    const ManagedMeeting* meeting = findMeeting(key);
    if (!meeting || meetingBusy_)
        return;

    appendLog(QString("Switching Chrome to \"%1\".").arg(meeting->title));
    setMeetingBusy(true, "Switching Chrome to the meeting tab...");
    emit activationRequested(
        meeting->handle(), meeting->tabRuntimeId, meeting->automationName(), key);
}

void MainWindow::readMicrophone(const QString& key)
{
    const ManagedMeeting* meeting = findMeeting(key);
    if (!meeting || meetingBusy_)
        return;

    appendLog(QString("Reading the microphone control of \"%1\".").arg(meeting->title));
    setMeetingBusy(true, "Reading Zoom's microphone control...");
    emit microphoneReadRequested(
        meeting->handle(), meeting->tabRuntimeId, meeting->automationName(), key);
}

void MainWindow::setMicrophone(const QString& key, bool muted)
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

void MainWindow::readSound(const QString& key)
{
    const ManagedMeeting* meeting = findMeeting(key);
    if (!meeting || meetingBusy_)
        return;
    appendLog(QString("Reading computer audio for \"%1\".").arg(meeting->title));
    setMeetingBusy(true, "Reading Zoom's sound and microphone states...");
    emit audioReadRequested(meeting->handle(), meeting->tabRuntimeId, meeting->automationName(), key);
}

void MainWindow::setSound(const QString& key, bool on)
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

void MainWindow::onMeetingActivated(const QJsonObject& result)
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

void MainWindow::applyMicrophoneResult(const QJsonObject& result, const QString& action)
{
    const QString key = result.value("requestKey").toString();
    ManagedMeeting* meeting = findMeeting(key);

    if (!result.value("ok").toBool())
    {
        const QString error = result.value("error").toString();
        appendLog(action + " failed: " + error);

        if (meeting)
            updateMeetingStates(*meeting, result);

        setMeetingBusy(false, action + " failed");
        QMessageBox::warning(this, action + " failed", error);
        return;
    }

    const QString state = result.value("state").toString();

    if (meeting)
    {
        updateMeetingStates(*meeting, result);
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

void MainWindow::onMicrophoneRead(const QJsonObject& result)
{
    applyMicrophoneResult(result, "Reading the microphone");
}

void MainWindow::onMicrophoneSet(const QJsonObject& result)
{
    applyMicrophoneResult(
        result, result.value("requestedMuted").toBool() ? "Muting" : "Unmuting");
}

void MainWindow::updateMeetingStates(ManagedMeeting& meeting, const QJsonObject& result)
{
    const QString runtimeId = result.value("tabRuntimeId").toString();
    if (!runtimeId.isEmpty())
        meeting.tabRuntimeId = runtimeId;
    const QString chromeName = result.value("tabName").toString();
    if (!chromeName.isEmpty())
    {
        if (meeting.title.isEmpty() || meeting.title == meeting.chromeTabName)
            meeting.title = chromeName;
        meeting.chromeTabName = chromeName;
    }

    if (!result.value("ok").toBool()
        || (result.contains("verified") && !result.value("verified").toBool()))
    {
        meeting.microphone = Microphone::Unknown;
        meeting.sound = Sound::Unknown;
        return;
    }

    const QString state = result.value("state").toString();
    meeting.microphone = state == "muted" ? Microphone::Muted
        : state == "unmuted" ? Microphone::Unmuted
        : state == "audioNotConnected" ? Microphone::AudioNotConnected : Microphone::Unknown;

    if (result.contains("audioState"))
    {
        const QString audioState = result.value("audioState").toString();
        meeting.sound = audioState == "connected" ? Sound::On
            : audioState == "disconnected" ? Sound::Off : Sound::Unknown;
    }
    else
    {
        // A microphone action does not change a previously verified computer
        // connection. Never infer sound-on from a microphone label by itself.
        if (meeting.microphone == Microphone::AudioNotConnected)
            meeting.sound = Sound::Off;
        else if (meeting.microphone == Microphone::Unknown || meeting.sound != Sound::On)
            meeting.sound = Sound::Unknown;
    }
}

bool MainWindow::meetingCardIsActive(const ManagedMeeting& meeting) const
{
    return meeting.sound == Sound::On || meeting.sound == Sound::Off;
}

void MainWindow::applyAudioResult(const QJsonObject& result, const QString& action)
{
    ManagedMeeting* meeting = findMeeting(result.value("requestKey").toString());
    const QString audioState = result.value("audioState").toString();
    const bool verified = result.value("ok").toBool() && result.value("verified").toBool()
        && (audioState == "connected" || audioState == "disconnected");
    QJsonObject observation = result;
    observation.insert("ok", verified);
    if (meeting)
        updateMeetingStates(*meeting, observation);

    if (!verified)
    {
        const QString error = result.value("error").toString("Zoom's sound state could not be verified.");
        const QString title = meeting ? meeting->title : QString("Meeting");
        appendLog(QString("\"%1\", %2 failed: %3").arg(title, action, error));
        setMeetingBusy(false, action + " failed · sound state unread");
        QMessageBox::warning(this, action + " failed", error);
        return;
    }

    const QString title = meeting ? meeting->title : QString("meeting");
    appendLog(QString("\"%1\" sound is %2 (verified from \"%3\"); microphone is %4.")
                  .arg(title, audioState == "connected" ? "on" : "off",
                       result.value("audioControlName").toString(), result.value("state").toString()));
    setMeetingBusy(false,
        QString("%1 · Sound %2 · microphone state refreshed")
            .arg(title, audioState == "connected" ? "on" : "off"));
}

void MainWindow::onAudioConnectionRead(const QJsonObject& result)
{
    applyAudioResult(result, "Reading sound");
}

void MainWindow::onAudioConnectionSet(const QJsonObject& result)
{
    applyAudioResult(result, result.value("requestedConnected").toBool()
        ? "Turning sound on" : "Turning sound off");
}

void MainWindow::requestRefreshConnections()
{
    startMeetingBatch(MeetingBatch::Operation::Refresh);
}

void MainWindow::requestSoundOffAll()
{
    startMeetingBatch(MeetingBatch::Operation::SoundOff);
}

void MainWindow::requestMuteAll()
{
    startMeetingBatch(MeetingBatch::Operation::Mute);
}

void MainWindow::startMeetingBatch(MeetingBatch::Operation operation)
{
    if (meetings_.isEmpty() || meetingBusy_ || pickupInProgress_)
        return;

    QJsonArray targets;
    for (const ManagedMeeting& meeting : std::as_const(meetings_))
    {
        targets.append(QJsonObject{
            {"windowHandle", meeting.windowHandle}, {"tabRuntimeId", meeting.tabRuntimeId},
            {"tabName", meeting.automationName()}, {"requestKey", meeting.key()}
        });
    }
    batchOperation_ = operation;
    batchChanged_ = 0;
    batchAlready_ = 0;
    batchFailures_.clear();
    const QString action = operation == MeetingBatch::Operation::Mute ? "Mute All"
        : operation == MeetingBatch::Operation::SoundOff ? "Sound Off All" : "Connection check";
    const QString message = QString("%1 · %2 meeting(s)").arg(action).arg(targets.size());
    appendLog(message);
    setMeetingBusy(true, message);
    batch_->start(operation, targets);
}

void MainWindow::onBatchMeetingFinished(const QJsonObject& result)
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
        updateMeetingStates(*meeting, observation);
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

void MainWindow::onBatchFinished(int skipped)
{
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

void MainWindow::appendLog(const QString& message)
{
    if (!liveDiagnostics_)
        return;

    const QString timestamp =
        QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    liveDiagnostics_->appendPlainText(QString("[%1] %2").arg(timestamp, message));
}

void MainWindow::applyDarkProfessionalTheme()
{
    setStyleSheet(R"QSS(
        QWidget#appRoot, QWidget#pageRoot {
            background: #0B1423;
            color: #E5E7EB;
            font-family: "Segoe UI";
            font-size: 10pt;
        }
        QFrame#pageBar {
            background: #0D1A2B;
            border-bottom: 1px solid #26364A;
        }
        QLabel#pageSubtitle, QLabel#mutedText {
            color: #9BAEC4;
            font-size: 9pt;
        }
        QPushButton#pageNavButton {
            color: #D6E0EC;
            background: transparent;
            border: 1px solid transparent;
            border-radius: 8px;
            padding: 8px 14px;
            font-size: 10pt;
            font-weight: 600;
        }
        QPushButton#pageNavButton:hover:enabled {
            background: #172A42;
        }
        QPushButton#pageNavButton:checked {
            color: white;
            background: #1265D5;
            border: 1px solid #2783F5;
        }
        QLabel#pageTitle {
            color: #F8FAFC;
            font-size: 24pt;
            font-weight: 700;
        }
        QLabel#sectionTitle {
            color: #EAF1F8;
            font-size: 13pt;
            font-weight: 700;
        }
        QFrame#metricCard {
            background: #17263A;
            border: 1px solid #2A4059;
            border-radius: 11px;
        }
        QLabel#metricHeading {
            color: #C7D4E4;
            font-size: 9pt;
        }
        QLabel#metricValue {
            color: #F8FAFC;
            font-size: 15pt;
            font-weight: 700;
        }
        QLabel#metricDetail {
            color: #8FA4BB;
            font-size: 8pt;
        }
        QPushButton {
            color: #E5EDF7;
            background: #1C2D43;
            border: 1px solid #354B66;
            border-radius: 8px;
            padding: 9px 13px;
            font-weight: 600;
        }
        QPushButton:hover:enabled { background: #263D59; }
        QPushButton:disabled {
            color: #65758A;
            background: #172335;
            border-color: #28394E;
        }
        QPushButton#uiaRefreshButton {
            padding: 4px 10px;
            min-height: 28px;
            max-height: 32px;
            font-size: 9pt;
        }
        QPushButton#primaryButton {
            color: white;
            background: #0878F9;
            border: 1px solid #278BFF;
            font-size: 10pt;
            font-weight: 700;
        }
        QPushButton#primaryButton:hover:enabled { background: #0867D8; }
        QPushButton#secondaryButton, QPushButton#soundButton, QPushButton#soundOffAllButton {
            background: #1D3048;
        }
        QWidget#meetingsContent { background: #0B1422; }
        QPushButton#dangerButton {
            color: #FCA5A5;
            background: #291D29;
            border: 1px solid #9F3145;
        }
        QLabel#countPill {
            color: #93C5FD;
            background: #142D4B;
            border: 1px solid #27588D;
            border-radius: 9px;
            padding: 3px 8px;
            font-weight: 700;
        }
        QFrame#meetingCard {
            background: #17263A;
            border: 1px solid transparent;
            border-radius: 11px;
        }
        QFrame#meetingCard[hovered="true"] {
            background: #1E334C;
            border: 1px solid #6EAFFF;
        }
        QFrame#meetingCardActive {
            background: #152B46;
            border: 1px solid transparent;
            border-radius: 11px;
        }
        QFrame#meetingCardActive[hovered="true"] {
            background: #1A3A5C;
            border: 1px solid #7CB6FF;
        }
        QFrame#meetingCard[dropHover="true"],
        QFrame#meetingCardActive[dropHover="true"] {
            border: 2px dashed #8EBEFF;
        }
        QLineEdit#meetingTitle {
            color: #F8FAFC;
            background: transparent;
            border: 1px solid transparent;
            border-radius: 4px;
            padding: 1px 2px;
            font-size: 11pt;
            font-weight: 700;
            selection-background-color: #145FC2;
        }
        QLineEdit#meetingTitle:hover {
            border: 1px solid #3A5674;
        }
        QLineEdit#meetingTitle:focus {
            color: #F8FAFC;
            background: #0E1C2E;
            border: 1px solid #2E8BF5;
        }
        QLabel#meetingIcon {
            color: white;
            background: #1765C5;
            border-radius: 9px;
            font-size: 15pt;
            font-weight: 700;
        }
        QLabel#profileHeading {
            color: #93C5FD;
            font-size: 10pt;
            font-weight: 700;
            padding-top: 8px;
        }
        QWidget#activeBadgeShell {
            background: #22C55E;
            border-radius: 9px;
        }
        QLabel#activeBadge {
            color: #052E1A;
            background: transparent;
            padding: 2px 0;
            font-size: 8pt;
            font-weight: 700;
        }
        QPushButton#activeRefreshButton {
            color: #052E1A;
            background: transparent;
            border: 1px solid transparent;
            border-radius: 8px;
            padding: 0;
            font-size: 9pt;
            font-weight: 700;
        }
        QPushButton#activeRefreshButton:hover:enabled {
            color: white;
            background: #15803D;
            border-color: #166534;
        }
        QPushButton#activeRefreshButton:disabled {
            color: #166534;
            background: transparent;
            border-color: transparent;
        }
        QPushButton#cardCloseButton {
            color: #C7D4E4;
            background: transparent;
            border: 1px solid transparent;
            border-radius: 8px;
            padding: 0;
            font-size: 14pt;
            font-weight: 700;
        }
        QPushButton#cardCloseButton:hover:enabled {
            color: #FCA5A5;
            background: #291D29;
            border: 1px solid #9F3145;
        }
        QPushButton#cardCloseButton:disabled {
            color: #64748B;
            background: transparent;
            border-color: transparent;
        }
        QLabel#micMuted { color: #4ADE80; font-weight: 700; }
        QLabel#micUnmuted { color: #FBBF24; font-weight: 700; }
        QLabel#micUnknown { color: #94A3B8; font-weight: 700; }
        QLabel#soundState { font-weight: 700; }
        QLabel#soundState[state="connected"] { color: #93C5FD; }
        QLabel#soundState[state="disconnected"] { color: #94A3B8; }
        QLabel#soundState[state="unknown"] { color: #FBBF24; }
        QFrame#emptyMeetingsCard {
            background: #111F31;
            border: 1px dashed #3A506A;
            border-radius: 12px;
        }
        QLabel#emptyIcon {
            color: #6EAFFF;
            font-size: 30pt;
        }
        QLabel#emptyTitle {
            color: #E8F0FA;
            font-size: 15pt;
            font-weight: 700;
        }
        QFrame#panel {
            background: #17263A;
            border: 1px solid #2A4059;
            border-radius: 11px;
        }
        QLabel#panelHeading {
            color: #C7D4E4;
            font-size: 9pt;
            font-weight: 700;
            letter-spacing: 1px;
        }
        QLabel#settingsBody {
            color: #D4DFEC;
            font-size: 10pt;
        }
        QComboBox {
            color: #E5EDF7;
            background: #101D2D;
            border: 1px solid #354B66;
            border-radius: 7px;
            padding: 8px;
        }
        QComboBox QAbstractItemView {
            color: #E5EDF7;
            background: #17263A;
            selection-background-color: #145FC2;
        }
        QTabWidget::pane {
            background: #101D2D;
            border: 1px solid #2A4059;
            border-radius: 8px;
            top: -1px;
        }
        QTabBar::tab {
            color: #9BAEC4;
            background: #142338;
            border: 1px solid #2A4059;
            padding: 9px 14px;
            margin-right: 3px;
        }
        QTabBar::tab:selected {
            color: #F8FAFC;
            background: #203A58;
            border-bottom-color: #2587F7;
        }
        QPlainTextEdit {
            color: #C9D7E8;
            background: #0A1320;
            border: none;
            selection-background-color: #145FC2;
            font-family: Consolas;
            font-size: 9pt;
        }
        QScrollArea { background: transparent; border: none; }
        QScrollBar:vertical {
            background: #101D2D;
            width: 10px;
            margin: 2px;
        }
        QScrollBar::handle:vertical {
            background: #354B66;
            border-radius: 4px;
            min-height: 28px;
        }
        QDialog#tabImportDialog {
            background: #0B1423;
            color: #E5E7EB;
        }
        QWidget#tabImportRow {
            background: #17263A;
            border: 1px solid #2A4059;
            border-radius: 8px;
        }
        QCheckBox#tabImportCheck {
            color: #F8FAFC;
            font-weight: 700;
            spacing: 10px;
        }
        QCheckBox::indicator {
            width: 16px;
            height: 16px;
            border: 1px solid #4C6480;
            border-radius: 4px;
            background: #101D2D;
        }
        QCheckBox::indicator:checked {
            background: #0878F9;
            border-color: #278BFF;
        }
        QStatusBar {
            color: #91A5BC;
            background: #0A1320;
            border-top: 1px solid #26364A;
        }
    )QSS");
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    event->accept();
}
