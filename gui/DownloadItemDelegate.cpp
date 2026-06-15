#include "DownloadItemDelegate.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include "Theme.h"
#include "fdm/store/Database.h"
#include "fdm/store/DownloadListModel.h"

namespace fdm_gui {

using fdm::store::DownloadListModel;
using fdm::store::DownloadStatus;

namespace {

// --- layout constants (logical pixels) -----------------------------------
constexpr int kLiveRowHeight = 60;
constexpr int kDoneRowHeight = 50;
constexpr int kPadX = 12;
constexpr int kIconSize = 30;
constexpr int kButtonSize = 28;     // hit target; glyph drawn smaller inside
constexpr int kButtonSpacing = 4;
constexpr int kBarHeight = 5;

bool isLive(DownloadStatus s) {
    return s != DownloadStatus::Completed && s != DownloadStatus::Failed;
}

QString humanBytes(qint64 bytes) {
    if (bytes < 0) return QStringLiteral("—");
    constexpr qint64 KiB = 1024;
    constexpr qint64 MiB = 1024 * KiB;
    constexpr qint64 GiB = 1024 * MiB;
    if (bytes >= GiB)
        return QString::number(static_cast<double>(bytes) / GiB, 'f', 2) + " GiB";
    if (bytes >= MiB)
        return QString::number(static_cast<double>(bytes) / MiB, 'f', 1) + " MiB";
    if (bytes >= KiB)
        return QString::number(static_cast<double>(bytes) / KiB, 'f', 1) + " KiB";
    return QString::number(bytes) + " B";
}

QString humanEta(qint64 seconds) {
    if (seconds < 60) return QString::number(seconds) + "s";
    if (seconds < 3600)
        return QString("%1m %2s").arg(seconds / 60).arg(seconds % 60);
    return QString("%1h %2m").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

QString statusLabel(DownloadStatus s) {
    switch (s) {
        case DownloadStatus::Queued:     return "Queued";
        case DownloadStatus::Active:     return "Downloading";
        case DownloadStatus::Paused:     return "Paused";
        case DownloadStatus::Finalizing: return "Finalizing…";
        case DownloadStatus::Completed:  return "Completed";
        case DownloadStatus::Failed:     return "Failed";
    }
    return "?";
}

// The inline buttons a row offers, in visual (left-to-right) order.
// `canResume` = a Failed row has resumable chunk state (CanResumeRole).
QList<QPair<DownloadItemDelegate::RowAction, icons::Glyph>> buttonsFor(
    DownloadStatus s, bool canResume) {
    using RA = DownloadItemDelegate::RowAction;
    switch (s) {
        case DownloadStatus::Active:
            return {{RA::Pause, icons::Glyph::Pause}, {RA::Cancel, icons::Glyph::Cancel}};
        case DownloadStatus::Paused:
            return {{RA::Resume, icons::Glyph::Play}, {RA::Cancel, icons::Glyph::Cancel}};
        case DownloadStatus::Queued:
        case DownloadStatus::Finalizing:
            return {{RA::Cancel, icons::Glyph::Cancel}};
        case DownloadStatus::Failed:
            if (canResume) {
                return {{RA::Resume, icons::Glyph::Play},
                        {RA::Retry, icons::Glyph::Retry}};
            }
            return {{RA::Retry, icons::Glyph::Retry}};
        case DownloadStatus::Completed:
            return {{RA::OpenFolder, icons::Glyph::Folder}};
    }
    return {};
}

// Convenience: pull both inputs off the model index.
QList<QPair<DownloadItemDelegate::RowAction, icons::Glyph>> buttonsFor(
    const QModelIndex& index) {
    const auto status =
        static_cast<DownloadStatus>(index.data(DownloadListModel::StatusRole).toInt());
    return buttonsFor(status, index.data(DownloadListModel::CanResumeRole).toBool());
}

QRect buttonRect(const QRect& rowRect, int buttonIndex, int buttonCount) {
    const int x = rowRect.right() - kPadX -
                  (buttonCount - buttonIndex) * (kButtonSize + kButtonSpacing) +
                  kButtonSpacing;
    return QRect(x, rowRect.center().y() - kButtonSize / 2, kButtonSize, kButtonSize);
}

// File-type icon from the mime database (matched by extension; the file may
// not exist yet). Falls back to painted generic glyphs.
QIcon fileIcon(const QString& path, const QString& kind) {
    if (kind == "video") return icons::icon(icons::Glyph::Video, theme::accent());
    static QMimeDatabase mimeDb;
    const QMimeType mime =
        mimeDb.mimeTypeForFile(path, QMimeDatabase::MatchExtension);
    QIcon themed = QIcon::fromTheme(mime.iconName());
    if (themed.isNull()) themed = QIcon::fromTheme(mime.genericIconName());
    if (!themed.isNull()) return themed;
    return icons::icon(icons::Glyph::File, theme::accent());
}

QColor dimText(const QStyleOptionViewItem& option) {
    QColor c = option.palette.color(QPalette::Text);
    c.setAlphaF(0.55);
    return c;
}

}  // namespace

DownloadItemDelegate::DownloadItemDelegate(QObject* parent)
    : QStyledItemDelegate(parent) {}

QSize DownloadItemDelegate::sizeHint(const QStyleOptionViewItem& option,
                                     const QModelIndex& index) const {
    const auto status =
        static_cast<DownloadStatus>(index.data(DownloadListModel::StatusRole).toInt());
    return QSize(option.rect.width(), isLive(status) ? kLiveRowHeight : kDoneRowHeight);
}

void DownloadItemDelegate::paint(QPainter* p, const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);

    const auto status =
        static_cast<DownloadStatus>(index.data(DownloadListModel::StatusRole).toInt());
    const bool live = isLive(status);
    const QRect r = option.rect;

    // --- row background ----------------------------------------------------
    const QRect cardRect = r.adjusted(4, 2, -4, -2);
    if (option.state & QStyle::State_Selected) {
        QColor bg = theme::accent();
        bg.setAlphaF(theme::isDark() ? 0.25 : 0.15);
        p->setPen(Qt::NoPen);
        p->setBrush(bg);
        p->drawRoundedRect(cardRect, 7, 7);
    } else if (option.state & QStyle::State_MouseOver) {
        QColor bg = option.palette.color(QPalette::Text);
        bg.setAlphaF(0.05);
        p->setPen(Qt::NoPen);
        p->setBrush(bg);
        p->drawRoundedRect(cardRect, 7, 7);
    }

    // --- file icon -----------------------------------------------------------
    const QString path = index.data(DownloadListModel::OutputPathRole).toString();
    const QString kind = index.data(DownloadListModel::KindRole).toString();
    const QRect iconRect(r.left() + kPadX, r.center().y() - kIconSize / 2,
                         kIconSize, kIconSize);
    fileIcon(path, kind).paint(p, iconRect);

    // --- inline buttons ------------------------------------------------------
    const auto buttons = buttonsFor(index);
    for (int i = 0; i < buttons.size(); ++i) {
        const QRect br = buttonRect(r, i, buttons.size());
        const bool hovered = (hoverRow_ == index.row() && hoverButton_ == i);
        if (hovered) {
            QColor bg = option.palette.color(QPalette::Text);
            bg.setAlphaF(0.12);
            p->setPen(Qt::NoPen);
            p->setBrush(bg);
            p->drawEllipse(br);
        }
        QColor glyphColor = option.palette.color(QPalette::Text);
        if (!hovered) glyphColor.setAlphaF(0.65);
        icons::icon(buttons[i].second, glyphColor)
            .paint(p, br.adjusted(6, 6, -6, -6));
    }

    // --- text area -----------------------------------------------------------
    const int textX = iconRect.right() + 12;
    const int buttonsWidth =
        buttons.isEmpty() ? 0 : buttons.size() * (kButtonSize + kButtonSpacing) + 8;
    const int textW = r.right() - kPadX - buttonsWidth - textX;
    if (textW <= 20) {
        p->restore();
        return;
    }

    const QString name = index.data(DownloadListModel::NameRole).toString();
    QFont nameFont = option.font;
    nameFont.setWeight(QFont::DemiBold);

    const qint64 received = index.data(DownloadListModel::BytesReceivedRole).toLongLong();
    const qint64 total = index.data(DownloadListModel::TotalBytesRole).toLongLong();
    const double speed = index.data(DownloadListModel::SpeedRole).toDouble();

    if (live) {
        // Line 1: name (left) + live stats (right).
        QString info;
        if (status == DownloadStatus::Active) {
            QStringList parts;
            if (speed > 0) parts << humanBytes(static_cast<qint64>(speed)) + "/s";
            if (speed > 0 && total > 0 && total > received) {
                parts << "ETA " + humanEta(static_cast<qint64>((total - received) / speed));
            }
            const int conns = index.data(DownloadListModel::ActiveConnectionsRole).toInt();
            if (conns > 0) parts << QString("%1 conn").arg(conns);
            info = parts.join(" · ");
            if (info.isEmpty()) info = "Starting…";
        } else {
            info = statusLabel(status);
        }

        p->setFont(option.font);
        const int infoW = qMin(p->fontMetrics().horizontalAdvance(info) + 4, textW / 2);
        const QRect infoRect(textX + textW - infoW, r.top() + 9, infoW, 18);
        p->setPen(status == DownloadStatus::Paused ? theme::statusColor(status)
                                                   : dimText(option));
        p->drawText(infoRect, Qt::AlignRight | Qt::AlignVCenter,
                    p->fontMetrics().elidedText(info, Qt::ElideRight, infoRect.width()));

        p->setFont(nameFont);
        p->setPen(option.palette.color(QPalette::Text));
        const QRect nameRect(textX, r.top() + 9, textW - infoW - 8, 18);
        p->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                    p->fontMetrics().elidedText(name, Qt::ElideMiddle, nameRect.width()));

        // Line 2: progress bar + percentage (or bytes when size unknown).
        QString pctText;
        double fraction = -1.0;
        if (total > 0) {
            fraction = qBound(0.0, static_cast<double>(received) / total, 1.0);
            pctText = QString::number(fraction * 100.0, 'f', 1) + "%";
        } else {
            pctText = received > 0 ? humanBytes(received) : "…";
        }

        p->setFont(option.font);
        const int pctW = p->fontMetrics().horizontalAdvance("100.0% of 999.9 MiB");
        // Clamp for very narrow windows: keep some bar visible and let the
        // trailing text elide instead of inverting the rect.
        const int barW = qMax(40, textW - pctW - 10);
        const QRect barRect(textX, r.bottom() - 20, barW, kBarHeight);

        QColor track = option.palette.color(QPalette::Text);
        track.setAlphaF(0.15);
        p->setPen(Qt::NoPen);
        p->setBrush(track);
        p->drawRoundedRect(barRect, kBarHeight / 2.0, kBarHeight / 2.0);
        if (fraction > 0) {
            QRect fill = barRect;
            fill.setWidth(qMax(kBarHeight, static_cast<int>(barRect.width() * fraction)));
            p->setBrush(theme::statusColor(status));
            p->drawRoundedRect(fill, kBarHeight / 2.0, kBarHeight / 2.0);
        }

        if (total > 0) {
            pctText += " of " + humanBytes(total);
        }
        p->setPen(dimText(option));
        const QRect pctRect(barRect.right() + 10, r.bottom() - 28,
                            qMax(0, textX + textW - barRect.right() - 10), 18);
        p->drawText(pctRect, Qt::AlignLeft | Qt::AlignVCenter,
                    p->fontMetrics().elidedText(pctText, Qt::ElideRight, pctRect.width()));
    } else {
        // Line 1: name (left) + status pill (right).
        const QString pill = statusLabel(status);
        p->setFont(option.font);
        const int pillTextW = p->fontMetrics().horizontalAdvance(pill);
        const QRect pillRect(textX + textW - pillTextW - 18, r.top() + 8,
                             pillTextW + 18, 19);
        QColor pillBg = theme::statusColor(status);
        pillBg.setAlphaF(theme::isDark() ? 0.22 : 0.14);
        p->setPen(Qt::NoPen);
        p->setBrush(pillBg);
        p->drawRoundedRect(pillRect, 9, 9);
        p->setPen(theme::statusColor(status));
        p->drawText(pillRect, Qt::AlignCenter, pill);

        p->setFont(nameFont);
        p->setPen(option.palette.color(QPalette::Text));
        const QRect nameRect(textX, r.top() + 8, textW - pillRect.width() - 8, 19);
        p->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                    p->fontMetrics().elidedText(name, Qt::ElideMiddle, nameRect.width()));

        // Line 2: URL (left, dim) + size · finished date (right, dim).
        const QString url = index.data(DownloadListModel::UrlRole).toString();
        QStringList metaParts;
        if (total > 0) metaParts << humanBytes(total);
        const qint64 updatedAt = index.data(DownloadListModel::UpdatedAtRole).toLongLong();
        if (updatedAt > 0) {
            metaParts << QDateTime::fromSecsSinceEpoch(updatedAt).toString("MMM d");
        }
        const QString meta = metaParts.join(" · ");

        QFont smallFont = option.font;
        smallFont.setPointSizeF(option.font.pointSizeF() - 1);
        p->setFont(smallFont);
        p->setPen(dimText(option));
        const int metaW = p->fontMetrics().horizontalAdvance(meta) + 4;
        const QRect metaRect(textX + textW - metaW, r.bottom() - 26, metaW, 16);
        p->drawText(metaRect, Qt::AlignRight | Qt::AlignVCenter, meta);
        const QRect urlRect(textX, r.bottom() - 26, textW - metaW - 8, 16);
        p->drawText(urlRect, Qt::AlignLeft | Qt::AlignVCenter,
                    p->fontMetrics().elidedText(url, Qt::ElideRight, urlRect.width()));
    }

    p->restore();
}

bool DownloadItemDelegate::eventFilter(QObject* watched, QEvent* event) {
    const bool leftItems =
        event->type() == QEvent::Leave ||
        (event->type() == QEvent::MouseMove && [&] {
            auto* view = qobject_cast<QAbstractItemView*>(parent());
            return view && !view->indexAt(
                               static_cast<QMouseEvent*>(event)->pos()).isValid();
        }());
    if (leftItems && hoverRow_ != -1) {
        hoverRow_ = -1;
        hoverButton_ = -1;
        if (auto* view = qobject_cast<QAbstractItemView*>(parent())) {
            view->viewport()->update();
        }
    }
    return QStyledItemDelegate::eventFilter(watched, event);
}

bool DownloadItemDelegate::editorEvent(QEvent* event, QAbstractItemModel* /*model*/,
                                       const QStyleOptionViewItem& option,
                                       const QModelIndex& index) {
    const auto buttons = buttonsFor(index);

    const auto hitButton = [&](const QPoint& pos) {
        for (int i = 0; i < buttons.size(); ++i) {
            if (buttonRect(option.rect, i, buttons.size()).contains(pos)) return i;
        }
        return -1;
    };

    switch (event->type()) {
        case QEvent::MouseMove: {
            const auto* me = static_cast<QMouseEvent*>(event);
            const int hit = hitButton(me->pos());
            const int row = (hit >= 0) ? index.row() : -1;
            if (row != hoverRow_ || hit != hoverButton_) {
                hoverRow_ = row;
                hoverButton_ = hit;
                if (auto* view = qobject_cast<QAbstractItemView*>(parent())) {
                    view->viewport()->update();
                }
            }
            return false;  // don't eat moves; the view needs them for hover
        }
        case QEvent::MouseButtonPress: {
            const auto* me = static_cast<QMouseEvent*>(event);
            // Swallow presses on buttons so they don't disturb the selection.
            return me->button() == Qt::LeftButton && hitButton(me->pos()) >= 0;
        }
        case QEvent::MouseButtonRelease: {
            const auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() != Qt::LeftButton) break;
            const int hit = hitButton(me->pos());
            if (hit < 0) break;
            const qint64 id = index.data(DownloadListModel::IdRole).toLongLong();
            if (id != 0) emit actionTriggered(id, buttons[hit].first);
            return true;
        }
        case QEvent::MouseButtonDblClick: {
            // A fast double-click on a button must not open the details
            // window for the row underneath.
            const auto* me = static_cast<QMouseEvent*>(event);
            return me->button() == Qt::LeftButton && hitButton(me->pos()) >= 0;
        }
        default:
            break;
    }
    return false;
}

}  // namespace fdm_gui
