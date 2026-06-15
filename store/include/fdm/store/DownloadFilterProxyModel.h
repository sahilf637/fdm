#pragma once

#include <QSortFilterProxyModel>
#include <QString>
#include <QtGlobal>

namespace fdm::store {

// Filter/sort layer between DownloadListModel and the GUI list view. Rows
// pass when they match both the selected category (a status/kind bucket)
// and a free-text needle over filename + URL. Sort order pins unfinished
// downloads above finished ones, newest first within each group.
class DownloadFilterProxyModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    enum class Category {
        All,
        Downloading,  // Queued | Active | Finalizing
        Paused,
        Completed,
        Failed,
        Videos,       // kind == "video", any status
    };

    explicit DownloadFilterProxyModel(QObject* parent = nullptr);

    void setCategory(Category category);
    Category category() const { return category_; }

    // Case-insensitive substring match against filename and URL. Empty
    // clears the filter.
    void setSearchText(const QString& text);

    // DownloadManager id for a proxy row, or 0 if out of range.
    qint64 idForRow(int proxyRow) const;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
    Category category_ = Category::All;
    QString search_;
};

}  // namespace fdm::store
