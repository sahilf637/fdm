#pragma once

#include <QAbstractTableModel>
#include <QList>
#include <QtGlobal>

namespace fdm::store {

class DownloadManager;

// QAbstractTableModel adapter over DownloadManager. Subscribes to the
// manager's row* signals and translates them into the model's begin/end
// methods so attached views auto-refresh.
class DownloadListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColName = 0,
        ColSize,
        ColProgress,
        ColSpeed,
        ColStatus,
        ColUrl,
        ColCount
    };

    // Raw per-download values, valid on any column of a row. Consumed by the
    // filter proxy (category/search/sort) and the card delegate, which need
    // the underlying data rather than the formatted display strings.
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,             // QString: output file leaf name
        UrlRole,              // QString
        OutputPathRole,       // QString: full save path
        StatusRole,           // int: DownloadStatus
        KindRole,             // QString: "http" (also for empty) or "video"
        BytesReceivedRole,    // qint64
        TotalBytesRole,       // qint64: -1 when unknown
        SpeedRole,            // double: bytes/sec, 0 when not active
        CreatedAtRole,        // qint64: unix seconds
        UpdatedAtRole,        // qint64: unix seconds
        ErrorRole,            // QString: last error, empty if none
        ActiveConnectionsRole,// int
        ConnectionLimitRole,  // int
        CanResumeRole,        // bool: Failed row resumable from saved chunks
    };

    explicit DownloadListModel(DownloadManager* manager, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    // Returns the DownloadManager id for the given visual row, or 0 if out
    // of range.
    qint64 idForRow(int row) const;

private slots:
    void onRowAdded(qint64 id);
    void onRowChanged(qint64 id);
    void onRowRemoved(qint64 id);

private:
    DownloadManager* manager_;
    QList<qint64> ids_;  // mirrors manager->rows() order
};

}  // namespace fdm::store
