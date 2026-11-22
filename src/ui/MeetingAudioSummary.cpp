#include "MeetingAudioSummary.h"
#include "meetings/ManagedMeeting.h"

#include <QBrush>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMouseEvent>

namespace
{
enum Column { MeetingName, Microphone, Sound, ColumnCount };
constexpr int kVisibleRows = 4;
}

MeetingAudioSummary::MeetingAudioSummary(QWidget* parent)
    : QTableWidget(0, ColumnCount, parent)
{
    setObjectName("meetingAudioStatus");
    setAccessibleName(tr("Meeting audio status"));
    setHorizontalHeaderLabels({tr("Meeting"), tr("Microphone"), tr("Sound")});
    horizontalHeaderItem(Microphone)->setToolTip(tr("Last microphone state reported by Zoom."));
    horizontalHeaderItem(Sound)->setToolTip(tr("Last computer-audio connection reported by Zoom."));

    horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    horizontalHeader()->setHighlightSections(false);
    horizontalHeader()->setSectionResizeMode(MeetingName, QHeaderView::Stretch);
    horizontalHeader()->setSectionResizeMode(Microphone, QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(Sound, QHeaderView::ResizeToContents);
    verticalHeader()->hide();
    verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setSelectionMode(QAbstractItemView::NoSelection);
    setMouseTracking(true);
    setTabKeyNavigation(false);
    setWordWrap(false);
    setShowGrid(false);
    setAlternatingRowColors(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(this, &QTableWidget::itemClicked, this, &MeetingAudioSummary::activateMeetingName);
    hide();
}

void MeetingAudioSummary::activateMeetingName(QTableWidgetItem* cell)
{
    if (cell && cell->column() == MeetingName)
        emit meetingNameActivated(cell->data(Qt::UserRole).toString());
}

void MeetingAudioSummary::keyPressEvent(QKeyEvent* event)
{
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && currentColumn() == MeetingName) {
        activateMeetingName(currentItem());
        event->accept();
        return;
    }
    QTableWidget::keyPressEvent(event);
}

void MeetingAudioSummary::mouseMoveEvent(QMouseEvent* event)
{
    viewport()->setCursor(indexAt(event->position().toPoint()).column() == MeetingName
        ? Qt::PointingHandCursor : Qt::ArrowCursor);
    QTableWidget::mouseMoveEvent(event);
}

void MeetingAudioSummary::setMeetings(const QList<ManagedMeeting>& meetings)
{
    setRowCount(static_cast<int>(meetings.size()));
    for (int row = 0; row < rowCount(); ++row)
        updateMeeting(row, meetings.at(row));

    ensurePolished();
    verticalHeader()->setDefaultSectionSize(fontMetrics().height() + 8);
    const int rowsHeight = qMin(rowCount(), kVisibleRows) * verticalHeader()->defaultSectionSize();
    setFixedHeight(horizontalHeader()->sizeHint().height() + rowsHeight + 2 * frameWidth());
    setVisible(!meetings.isEmpty());
}

void MeetingAudioSummary::updateMeeting(int row, const ManagedMeeting& meeting)
{
    const auto setCell = [this, row](Column column, const QString& text, const QBrush& color = {}) {
        auto* cell = item(row, column);
        if (!cell) {
            cell = new QTableWidgetItem;
            setItem(row, column, cell);
        }
        cell->setText(text);
        cell->setForeground(color);
        return cell;
    };

    const QString title = meeting.displayTitle();
    auto* name = setCell(MeetingName, title, QColor("#A4C9F0"));
    name->setData(Qt::UserRole, meeting.key());
    const QString hint = tr("Move this meeting card to the top.");
    name->setData(Qt::AccessibleDescriptionRole, hint);
    QStringList details{title};
    if (!meeting.profileName.isEmpty())
        details.append(meeting.profileName);
    if (!meeting.meetingId.isEmpty())
        details.append(tr("Zoom Meeting %1").arg(meeting.meetingId));
    details.append(hint);
    name->setToolTip(details.join('\n'));

    switch (meeting.microphone)
    {
    case ManagedMeeting::Microphone::Muted:
        setCell(Microphone, tr("Muted"), QColor("#86DCA4"));
        break;
    case ManagedMeeting::Microphone::Unmuted:
        setCell(Microphone, tr("Unmuted"), QColor("#FFD178"));
        break;
    case ManagedMeeting::Microphone::AudioNotConnected:
        setCell(Microphone, tr("Disconnected"));
        break;
    case ManagedMeeting::Microphone::Unknown:
        setCell(Microphone, tr("Unknown"));
        break;
    }
    switch (meeting.sound)
    {
    case ManagedMeeting::Sound::On:
        setCell(Sound, tr("On"), QColor("#9DDBB4"));
        break;
    case ManagedMeeting::Sound::Off:
        setCell(Sound, tr("Off"));
        break;
    case ManagedMeeting::Sound::Unknown:
        setCell(Sound, tr("Unknown"));
        break;
    }
}
