#include "fdm/store/DownloadListModel.h"

#include <QFileInfo>

#include "fdm/store/DownloadManager.h"

namespace fdm::store {

namespace {

QString humanBytes(qint64 bytes) {
    if (bytes < 0) return "—";
    constexpr qint64 KiB = 1024;
    constexpr qint64 MiB = 1024 * KiB;
    constexpr qint64 GiB = 1024 * MiB;
    if (bytes >= GiB)
        return QString::number(static_cast<double>(bytes) / GiB, 'f', 2) + " GiB";
    if (bytes >= MiB)
        return QString::number(static_cast<double>(bytes) / MiB, 'f', 2) + " MiB";
    if (bytes >= KiB)
        return QString::number(static_cast<double>(bytes) / KiB, 'f', 1) + " KiB";
    return QString::number(bytes) + " B";
}

QString humanRate(double bytesPerSec) {
    if (bytesPerSec <= 0) return "—";
    return humanBytes(static_cast<qint64>(bytesPerSec)) + "/s";
}

QString statusLabel(DownloadStatus s) {
    switch (s) {
        case DownloadStatus::Queued:    return "Queued";
        case DownloadStatus::Active:    return "Downloading";
        case DownloadStatus::Paused:    return "Paused";
        case DownloadStatus::Completed: return "Completed";
        case DownloadStatus::Failed:    return "Failed";
    }
    return "?";
}

QString progressLabel(const DownloadLiveRow& row) {
    if (row.rec.totalBytes > 0) {
        const double pct = 100.0 * static_cast<double>(row.bytesReceived) /
                           static_cast<double>(row.rec.totalBytes);
        return QString::number(pct, 'f', 1) + "%";
    }
    return row.bytesReceived > 0 ? humanBytes(row.bytesReceived) : "—";
}

}  // namespace

DownloadListModel::DownloadListModel(DownloadManager* manager, QObject* parent)
    : QAbstractTableModel(parent), manager_(manager) {
    for (const DownloadLiveRow& row : manager_->rows()) {
        ids_.append(row.rec.id);
    }
    connect(manager_, &DownloadManager::rowAdded, this, &DownloadListModel::onRowAdded);
    connect(manager_, &DownloadManager::rowChanged, this, &DownloadListModel::onRowChanged);
    connect(manager_, &DownloadManager::rowRemoved, this, &DownloadListModel::onRowRemoved);
}

int DownloadListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return ids_.size();
}

int DownloadListModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return ColCount;
}

QVariant DownloadListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return {};
    if (index.row() < 0 || index.row() >= ids_.size()) return {};

    const qint64 id = ids_[index.row()];
    const auto opt = manager_->row(id);
    if (!opt) return {};
    const DownloadLiveRow& row = *opt;

    if (role == Qt::ToolTipRole) {
        if (index.column() == ColStatus && !row.rec.error.isEmpty()) {
            return row.rec.error;
        }
        return row.rec.url;
    }

    if (role != Qt::DisplayRole) return {};

    switch (index.column()) {
        case ColName: {
            const QString name = QFileInfo(row.rec.outputPath).fileName();
            return name.isEmpty() ? row.rec.outputPath : name;
        }
        case ColSize:
            return humanBytes(row.rec.totalBytes);
        case ColProgress:
            return progressLabel(row);
        case ColSpeed:
            return row.rec.status == DownloadStatus::Active ? humanRate(row.bytesPerSec) : "—";
        case ColStatus:
            return statusLabel(row.rec.status);
        case ColUrl:
            return row.rec.url;
    }
    return {};
}

QVariant DownloadListModel::headerData(int section, Qt::Orientation orientation,
                                       int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return {};
    switch (section) {
        case ColName:     return "Name";
        case ColSize:     return "Size";
        case ColProgress: return "Progress";
        case ColSpeed:    return "Speed";
        case ColStatus:   return "Status";
        case ColUrl:      return "URL";
    }
    return {};
}

qint64 DownloadListModel::idForRow(int row) const {
    if (row < 0 || row >= ids_.size()) return 0;
    return ids_[row];
}

void DownloadListModel::onRowAdded(qint64 id) {
    const int row = ids_.size();
    beginInsertRows(QModelIndex(), row, row);
    ids_.append(id);
    endInsertRows();
}

void DownloadListModel::onRowChanged(qint64 id) {
    const int row = ids_.indexOf(id);
    if (row < 0) return;
    emit dataChanged(index(row, 0), index(row, ColCount - 1));
}

void DownloadListModel::onRowRemoved(qint64 id) {
    const int row = ids_.indexOf(id);
    if (row < 0) return;
    beginRemoveRows(QModelIndex(), row, row);
    ids_.removeAt(row);
    endRemoveRows();
}

}  // namespace fdm::store
