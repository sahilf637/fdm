#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include <memory>

#include "fdm/DownloadEngine.h"
#include "fdm/EngineEvent.h"

namespace fdm_gui {

// One row of the chunk table. Mirrors fdm::ChunkProgress but with Qt-friendly
// types so the bridge signals can stay UI-side without any libcurl/std dependency.
struct ChunkRow {
    int index = 0;
    qint64 startByte = 0;
    qint64 endByte = -1;
    qint64 bytesReceived = 0;
    int attempts = 1;
    int status = 0;  // 0 = Active, 1 = Done, 2 = Failed
};

// QObject wrapper around fdm::DownloadEngine. Owns the engine and marshals its
// engine-thread callback onto the UI thread (via QMetaObject::invokeMethod with
// a queued connection) before emitting Qt signals. All signals fire on the
// thread that owns this bridge (the main thread, in our app).
class EngineBridge : public QObject {
    Q_OBJECT
public:
    explicit EngineBridge(QObject* parent = nullptr);
    ~EngineBridge() override;

    // Idempotent: ignored if a download is already in flight.
    void startDownload(const QString& url, const QString& outputPath);

    bool isRunning() const { return running_; }

signals:
    void started(qint64 totalBytes, bool supportsRanges, int chunkCount,
                 const QList<ChunkRow>& chunkRows);
    void progressUpdated(qint64 received, qint64 total, double bytesPerSec,
                         const QList<ChunkRow>& chunkRows);
    void downloadFinished();
    void downloadFailed(const QString& reason);

private:
    void handleEvent(fdm::EngineEvent ev);  // runs on the UI thread

    std::unique_ptr<fdm::DownloadEngine> engine_;
    bool running_ = false;
};

}  // namespace fdm_gui

Q_DECLARE_METATYPE(QList<fdm_gui::ChunkRow>)
