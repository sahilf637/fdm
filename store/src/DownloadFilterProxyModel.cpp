#include "fdm/store/DownloadFilterProxyModel.h"

#include "fdm/store/Database.h"
#include "fdm/store/DownloadListModel.h"

namespace fdm::store {

namespace {

// Unfinished downloads sort above terminal ones regardless of age.
bool isTerminal(DownloadStatus s) {
    return s == DownloadStatus::Completed || s == DownloadStatus::Failed;
}

}  // namespace

DownloadFilterProxyModel::DownloadFilterProxyModel(QObject* parent)
    : QSortFilterProxyModel(parent) {
    setDynamicSortFilter(true);
    sort(0);
}

void DownloadFilterProxyModel::setCategory(Category category) {
    if (category_ == category) return;
    category_ = category;
    invalidateRowsFilter();
}

void DownloadFilterProxyModel::setSearchText(const QString& text) {
    const QString trimmed = text.trimmed();
    if (search_ == trimmed) return;
    search_ = trimmed;
    invalidateRowsFilter();
}

qint64 DownloadFilterProxyModel::idForRow(int proxyRow) const {
    return index(proxyRow, 0).data(DownloadListModel::IdRole).toLongLong();
}

bool DownloadFilterProxyModel::filterAcceptsRow(int sourceRow,
                                                const QModelIndex& sourceParent) const {
    const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
    const auto status =
        static_cast<DownloadStatus>(idx.data(DownloadListModel::StatusRole).toInt());

    switch (category_) {
        case Category::All:
            break;
        case Category::Downloading:
            if (status != DownloadStatus::Queued && status != DownloadStatus::Active &&
                status != DownloadStatus::Finalizing) {
                return false;
            }
            break;
        case Category::Paused:
            if (status != DownloadStatus::Paused) return false;
            break;
        case Category::Completed:
            if (status != DownloadStatus::Completed) return false;
            break;
        case Category::Failed:
            if (status != DownloadStatus::Failed) return false;
            break;
        case Category::Videos:
            if (idx.data(DownloadListModel::KindRole).toString() != "video") return false;
            break;
    }

    if (search_.isEmpty()) return true;
    return idx.data(DownloadListModel::NameRole).toString().contains(search_,
                                                                     Qt::CaseInsensitive) ||
           idx.data(DownloadListModel::UrlRole).toString().contains(search_,
                                                                    Qt::CaseInsensitive);
}

bool DownloadFilterProxyModel::lessThan(const QModelIndex& left,
                                        const QModelIndex& right) const {
    const auto statusL =
        static_cast<DownloadStatus>(left.data(DownloadListModel::StatusRole).toInt());
    const auto statusR =
        static_cast<DownloadStatus>(right.data(DownloadListModel::StatusRole).toInt());
    if (isTerminal(statusL) != isTerminal(statusR)) return !isTerminal(statusL);

    // Newest first; id breaks ties (createdAt has 1-second granularity).
    const qint64 createdL = left.data(DownloadListModel::CreatedAtRole).toLongLong();
    const qint64 createdR = right.data(DownloadListModel::CreatedAtRole).toLongLong();
    if (createdL != createdR) return createdL > createdR;
    return left.data(DownloadListModel::IdRole).toLongLong() >
           right.data(DownloadListModel::IdRole).toLongLong();
}

}  // namespace fdm::store
