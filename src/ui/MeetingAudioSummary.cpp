#include "MeetingAudioSummary.h"
#include "AppTheme.h"
#include "meetings/ManagedMeeting.h"

#include <QBrush>
#include <QHeaderView>
#include <QMouseEvent>
#include <QStyledItemDelegate>

namespace
{
enum Column { MeetingName, Microphone, Sound, ColumnCount };
constexpr int kVisibleRows = 4;
constexpr int appearanceRole = Qt::UserRole + 1;
enum class Appearance { Normal, Link, Positive, Warning };

class AudioStatusDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        auto& colors = option->palette;
        switch (static_cast<Appearance>(index.data(appearanceRole).toInt())) {
        case Appearance::Link: colors.setBrush(QPalette::Text, colors.link()); break;
        case Appearance::Positive: colors.setColor(QPalette::Text, app_theme::positiveText(colors)); break;
        case Appearance::Warning:
            option->backgroundBrush = app_theme::warningBackground;
            colors.setBrush(QPalette::Text, colors.brightText());
            break;
        case Appearance::Normal: break;
        }
    }
};
}

MeetingAudioSummary::MeetingAudioSummary(QWidget* parent)
    : QTableWidget(0, ColumnCount, parent)
{
    setObjectName("meetingAudioStatus");
    setItemDelegate(new AudioStatusDelegate(this));
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
    connect(this, &QTableWidget::itemDoubleClicked, this, [this](const QTableWidgetItem* cell) {
        if (cell->column() == MeetingName)
            emit meetingNameDoubleClicked(cell->data(Qt::UserRole).toString());
    });
    hide();
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
    const bool unmuted = meeting.microphone == ManagedMeeting::Microphone::Unmuted;
    const auto setCell = [this, row, unmuted](Column column, const QString& text, Appearance appearance = Appearance::Normal) {
        auto* cell = item(row, column);
        if (!cell) {
            cell = new QTableWidgetItem;
            setItem(row, column, cell);
        }
        cell->setText(text);
        cell->setData(appearanceRole, static_cast<int>(unmuted ? Appearance::Warning : appearance));
        return cell;
    };

    const QString title = meeting.displayTitle();
    auto* name = setCell(MeetingName, title, Appearance::Link);
    name->setData(Qt::UserRole, meeting.key());
    const QString hint = tr("Double-click to move this meeting card to the top.");
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
        setCell(Microphone, tr("Muted"), Appearance::Positive);
        break;
    case ManagedMeeting::Microphone::Unmuted:
        setCell(Microphone, tr("Unmuted"));
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
        setCell(Sound, tr("On"), Appearance::Positive);
        break;
    case ManagedMeeting::Sound::Off:
        setCell(Sound, tr("Off"));
        break;
    case ManagedMeeting::Sound::Unknown:
        setCell(Sound, tr("Unknown"));
        break;
    }
}
