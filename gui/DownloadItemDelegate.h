#pragma once

#include <QStyledItemDelegate>
#include <QtGlobal>

#include "Icons.h"

namespace fdm_gui {

// Paints one download as a two-line card: file-type icon, bold name, then a
// progress bar + speed/ETA line for live rows or URL + size/date + status
// pill for finished ones. Inline icon buttons on the right (pause / resume /
// cancel / retry / open folder, depending on status) replace the old
// always-visible toolbar actions; clicks surface via actionTriggered().
class DownloadItemDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    enum class RowAction { Pause, Resume, Cancel, Retry, OpenFolder };

    explicit DownloadItemDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override;

    // Install on the view's viewport: clears the button-hover highlight when
    // the cursor leaves the rows (editorEvent only fires while over an item).
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    void actionTriggered(qint64 id, RowAction action);

private:
    // Button hover state, so the button under the cursor gets a highlight.
    // Keyed by (row, button position); row -1 means none.
    int hoverRow_ = -1;
    int hoverButton_ = -1;
};

}  // namespace fdm_gui
