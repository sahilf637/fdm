#include "EngineBridge.h"

#include <QMetaObject>

#include <utility>
#include <variant>

namespace fdm_gui {

namespace {

QList<ChunkRow> toRows(const std::vector<fdm::ChunkProgress>& chunks) {
    QList<ChunkRow> out;
    out.reserve(static_cast<int>(chunks.size()));
    for (const auto& c : chunks) {
        ChunkRow row;
        row.index = c.index;
        row.startByte = c.startByte;
        row.endByte = c.endByte;
        row.bytesReceived = c.bytesReceived;
        row.attempts = c.attempts;
        row.status = static_cast<int>(c.status);
        out.append(row);
    }
    return out;
}

QList<ChunkRow> rowsFromSpecs(const std::vector<fdm::ChunkSpec>& specs) {
    QList<ChunkRow> out;
    out.reserve(static_cast<int>(specs.size()));
    for (const auto& s : specs) {
        ChunkRow row;
        row.index = s.index;
        row.startByte = s.startByte;
        row.endByte = s.endByte;
        row.bytesReceived = 0;
        row.attempts = 1;
        row.status = 0;  // Active
        out.append(row);
    }
    return out;
}

}  // namespace

EngineBridge::EngineBridge(QObject* parent) : QObject(parent) {
    qRegisterMetaType<QList<ChunkRow>>("QList<fdm_gui::ChunkRow>");
    engine_ = std::make_unique<fdm::DownloadEngine>();
}

EngineBridge::~EngineBridge() = default;

void EngineBridge::startDownload(const QString& url, const QString& outputPath) {
    if (running_) return;
    running_ = true;
    engine_->start(url.toStdString(), outputPath.toStdString(),
                   [this](fdm::EngineEvent ev) {
                       // Runs on the engine thread. Hop to the UI thread before
                       // touching anything Qt-side.
                       QMetaObject::invokeMethod(
                           this,
                           [this, ev = std::move(ev)]() mutable {
                               handleEvent(std::move(ev));
                           },
                           Qt::QueuedConnection);
                   });
}

void EngineBridge::handleEvent(fdm::EngineEvent ev) {
    std::visit(
        [this](auto&& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, fdm::Started>) {
                emit started(e.contentLength, e.supportsRanges, e.chunkCount,
                             rowsFromSpecs(e.chunks));
            } else if constexpr (std::is_same_v<T, fdm::Progress>) {
                emit progressUpdated(e.received, e.total, e.bytesPerSec,
                                     toRows(e.chunks));
            } else if constexpr (std::is_same_v<T, fdm::Finished>) {
                running_ = false;
                emit downloadFinished();
            } else if constexpr (std::is_same_v<T, fdm::Failed>) {
                running_ = false;
                emit downloadFailed(QString::fromStdString(e.reason));
            }
        },
        ev);
}

}  // namespace fdm_gui
