#include "fdm/store/DownloadManager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QtConcurrent>

#include <memory>
#include <utility>
#include <variant>

#include "SidecarFetcher.h"

namespace fdm::store {

namespace {

QCryptographicHash::Algorithm qtAlgFor(const QString& alg) {
    if (alg == "sha256") return QCryptographicHash::Sha256;
    if (alg == "sha1")   return QCryptographicHash::Sha1;
    return QCryptographicHash::Md5;
}

}  // namespace


namespace {

constexpr qint64 kPersistThrottleMs = 2000;  // flush chunk progress at most every 2s

// Locate a helper binary: prefer one shipped next to the app (or in the build
// tree's host/ dir), else fall back to PATH. Empty if not found anywhere.
QString findHelper(const QString& name) {
    const QString dir = QCoreApplication::applicationDirPath();
    for (const QString& c : {dir + "/" + name, dir + "/../host/" + name}) {
        const QFileInfo fi(c);
        if (fi.exists() && fi.isExecutable()) return fi.absoluteFilePath();
    }
    return QStandardPaths::findExecutable(name);
}

// Strip path separators / control chars from a video title so it's safe as a
// leaf filename. Falls back to "video" and caps the length.
QString sanitizeLeaf(const QString& title) {
    QString out;
    for (const QChar c : title) {
        if (c == '/' || c == '\\') continue;
        if (c.unicode() < 0x20 || c.unicode() == 0x7F) continue;
        out += c;
    }
    out = out.trimmed();
    if (out.isEmpty()) out = QStringLiteral("video");
    return out.left(180);
}

ChunkPersistStatus toPersistStatus(fdm::ChunkProgress::Status s) {
    switch (s) {
        case fdm::ChunkProgress::Status::Done:    return ChunkPersistStatus::Done;
        case fdm::ChunkProgress::Status::Failed:  return ChunkPersistStatus::Failed;
        case fdm::ChunkProgress::Status::Pending: return ChunkPersistStatus::Pending;
        case fdm::ChunkProgress::Status::Active:  return ChunkPersistStatus::Active;
    }
    return ChunkPersistStatus::Active;
}

fdm::ChunkProgress::Status fromPersistStatus(ChunkPersistStatus s) {
    switch (s) {
        case ChunkPersistStatus::Done:    return fdm::ChunkProgress::Status::Done;
        case ChunkPersistStatus::Failed:  return fdm::ChunkProgress::Status::Failed;
        case ChunkPersistStatus::Pending: return fdm::ChunkProgress::Status::Pending;
        case ChunkPersistStatus::Active:  return fdm::ChunkProgress::Status::Active;
    }
    return fdm::ChunkProgress::Status::Active;
}

QList<ChunkRecord> chunksFromSpecs(const std::vector<fdm::ChunkSpec>& specs) {
    QList<ChunkRecord> out;
    out.reserve(static_cast<int>(specs.size()));
    for (const fdm::ChunkSpec& s : specs) {
        ChunkRecord c;
        c.index = s.index;
        c.startByte = s.startByte;
        c.endByte = s.endByte;
        c.bytesReceived = 0;
        c.attempts = 1;
        c.status = ChunkPersistStatus::Active;
        out.append(c);
    }
    return out;
}

QList<ChunkRecord> chunksFromProgress(const std::vector<fdm::ChunkProgress>& progress) {
    QList<ChunkRecord> out;
    out.reserve(static_cast<int>(progress.size()));
    for (const fdm::ChunkProgress& p : progress) {
        ChunkRecord c;
        c.index = p.index;
        c.startByte = p.startByte;
        c.endByte = p.endByte;
        c.bytesReceived = p.bytesReceived;
        c.attempts = p.attempts;
        c.status = toPersistStatus(p.status);
        out.append(c);
    }
    return out;
}

qint64 sumChunkBytes(const QList<ChunkRecord>& chunks) {
    qint64 sum = 0;
    for (const ChunkRecord& c : chunks) sum += c.bytesReceived;
    return sum;
}

// Header plumbing. The engine speaks raw "Name: value" lines; the DB stores
// them as a JSON array of those same lines (one round-trippable representation
// shared by both directions).
std::vector<std::string> toEngineHeaders(const QList<QPair<QString, QString>>& headers) {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(headers.size()));
    for (const auto& [name, value] : headers) {
        if (name.isEmpty()) continue;
        out.push_back((name + ": " + value).toStdString());
    }
    return out;
}

QString headersToJson(const QList<QPair<QString, QString>>& headers) {
    if (headers.isEmpty()) return {};
    QJsonArray arr;
    for (const auto& [name, value] : headers) {
        if (name.isEmpty()) continue;
        arr.append(name + ": " + value);
    }
    if (arr.isEmpty()) return {};
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

std::vector<std::string> headersFromJson(const QString& json) {
    std::vector<std::string> out;
    if (json.isEmpty()) return out;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isArray()) return out;
    for (const QJsonValue& v : doc.array()) {
        const QString line = v.toString();
        if (!line.isEmpty()) out.push_back(line.toStdString());
    }
    return out;
}

}  // namespace

DownloadManager::DownloadManager(Database* db, QObject* parent)
    : QObject(parent), db_(db) {
    // First: any download that was Active in the DB was interrupted by the
    // previous process exit (we are by definition not running yet). Demote
    // before loading the in-memory mirror so the UI sees them as Paused.
    db_->markInterruptedAsPaused();

    QList<qint64> toFinalize;
    for (const DownloadRecord& rec : db_->listDownloads()) {
        DownloadLiveRow row;
        row.rec = rec;
        row.chunks = db_->chunksFor(rec.id);
        row.bytesReceived = sumChunkBytes(row.chunks);
        rows_.insert(rec.id, row);
        rowOrder_.append(rec.id);
        if (rec.status == DownloadStatus::Finalizing) {
            toFinalize.append(rec.id);
        }
    }

    engine_ = std::make_unique<fdm::DownloadEngine>();

    // Pick up any downloads that crashed mid-verification: file bytes are
    // on disk, just re-run hash discovery + compute. Defer to the event
    // loop so this happens after the constructor's caller has finished
    // (avoids re-entrancy through signal connections that haven't been
    // wired yet by the caller).
    for (qint64 id : toFinalize) {
        QMetaObject::invokeMethod(
            this, [this, id]() { beginFinalize(id); }, Qt::QueuedConnection);
    }
}

DownloadManager::~DownloadManager() = default;

QList<DownloadLiveRow> DownloadManager::rows() const {
    QList<DownloadLiveRow> out;
    out.reserve(rowOrder_.size());
    for (qint64 id : rowOrder_) {
        auto it = rows_.find(id);
        if (it != rows_.end()) out.append(*it);
    }
    return out;
}

std::optional<DownloadLiveRow> DownloadManager::row(qint64 id) const {
    auto it = rows_.find(id);
    if (it == rows_.end()) return std::nullopt;
    return *it;
}

qint64 DownloadManager::idAt(int row) const {
    if (row < 0 || row >= rowOrder_.size()) return 0;
    return rowOrder_[row];
}

DownloadLiveRow* DownloadManager::find(qint64 id) {
    auto it = rows_.find(id);
    return it == rows_.end() ? nullptr : &*it;
}

qint64 DownloadManager::startNew(const QString& url, const QString& outputPath) {
    return startNew(url, outputPath, QString(), QString(), {});
}

qint64 DownloadManager::startNew(const QString& url, const QString& outputPath,
                                 const QString& userHash, const QString& userHashAlgorithm) {
    return startNew(url, outputPath, userHash, userHashAlgorithm, {});
}

qint64 DownloadManager::startNew(const QString& url, const QString& outputPath,
                                 const QString& userHash, const QString& userHashAlgorithm,
                                 const QList<QPair<QString, QString>>& headers) {
    DownloadRecord rec;
    rec.url = url;
    rec.outputPath = outputPath;
    rec.totalBytes = -1;
    rec.supportsRanges = false;
    rec.status = DownloadStatus::Active;
    rec.createdAt = QDateTime::currentSecsSinceEpoch();
    rec.requestHeaders = headersToJson(headers);
    if (!userHash.isEmpty() && !userHashAlgorithm.isEmpty()) {
        rec.expectedHash = userHash;
        rec.hashAlgorithm = userHashAlgorithm;
        rec.hashSource = "user";
        rec.verification = VerificationStatus::Pending;
    }
    const qint64 id = db_->insertDownload(rec);

    DownloadLiveRow row;
    row.rec = rec;
    row.rec.id = id;
    rows_.insert(id, row);
    rowOrder_.append(id);
    emit rowAdded(id);

    DownloadLiveRow* live = find(id);
    live->engineId = engine_->start(
        url.toStdString(), outputPath.toStdString(), toEngineHeaders(headers),
        [this, id](fdm::EngineEvent ev) {
            QMetaObject::invokeMethod(
                this, [this, id, ev = std::move(ev)]() mutable {
                    onEngineEvent(id, std::move(ev));
                },
                Qt::QueuedConnection);
        });
    return id;
}

qint64 DownloadManager::startVideo(const QString& url, const QString& selector,
                                   const QString& title, const QString& outputDir,
                                   const QList<QPair<QString, QString>>& headers) {
    DownloadRecord rec;
    rec.url = url;
    rec.kind = "video";
    rec.videoSelector =
        selector.isEmpty() ? QStringLiteral("bestvideo*+bestaudio/best") : selector;
    // Provisional path; yt-dlp reports the real extension after muxing and we
    // adopt it then (see finishVideo).
    rec.outputPath = QDir(outputDir).filePath(sanitizeLeaf(title) + ".mp4");
    rec.status = DownloadStatus::Active;
    rec.createdAt = QDateTime::currentSecsSinceEpoch();
    rec.requestHeaders = headersToJson(headers);
    const qint64 id = db_->insertDownload(rec);

    DownloadLiveRow row;
    row.rec = rec;
    row.rec.id = id;
    rows_.insert(id, row);
    rowOrder_.append(id);
    emit rowAdded(id);

    spawnVideo(id);
    return id;
}

void DownloadManager::spawnVideo(qint64 id) {
    DownloadLiveRow* row = find(id);
    if (!row) return;

    const QString ytdlp = findHelper("yt-dlp");
    if (ytdlp.isEmpty()) {
        finishVideo(id, false, "yt-dlp not found");
        return;
    }

    const QFileInfo out(row->rec.outputPath);
    const QString dir = out.absolutePath();

    QStringList args;
    args << "-f" << row->rec.videoSelector << "--no-playlist" << "--newline"
         << "--no-mtime" << "--no-simulate" << "--no-warnings"
         << "--merge-output-format" << "mp4" << "--progress-template"
         << "FDMPROG|%(progress.downloaded_bytes)s|%(progress.total_bytes)s|"
            "%(progress.total_bytes_estimate)s|%(progress.speed)s"
         << "--print" << "after_move:filepath"
         // Name by the video's real title (finishVideo adopts the actual path).
         // The provisional row name still shows whatever title we were given.
         << "-o" << QDir(dir).filePath("%(title)s.%(ext)s");
    const QString ffmpeg = findHelper("ffmpeg");
    if (!ffmpeg.isEmpty()) args << "--ffmpeg-location" << ffmpeg;
    // Forward UA and any extra headers, but NOT cookies: a logged-in YouTube
    // session forces the PO-token-gated web client, which yields no usable
    // formats ("Requested format is not available").
    for (const std::string& h : headersFromJson(row->rec.requestHeaders)) {
        const QString line = QString::fromStdString(h);
        const int sep = line.indexOf(": ");
        if (sep < 0) continue;
        const QString name = line.left(sep);
        if (name.compare("Cookie", Qt::CaseInsensitive) == 0) continue;
        if (name.compare("User-Agent", Qt::CaseInsensitive) == 0)
            args << "--user-agent" << line.mid(sep + 2);
        else
            args << "--add-header" << line;
    }
    args << "--" << row->rec.url;

    auto* proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    videoProcs_.insert(id, proc);
    videoFinalPath_.remove(id);

    connect(proc, &QProcess::readyReadStandardOutput, this, [this, id, proc]() {
        while (proc->canReadLine()) {
            const QString line = QString::fromUtf8(proc->readLine()).trimmed();
            if (!line.isEmpty()) onVideoLine(id, line);
        }
    });
    connect(proc, &QProcess::finished, this,
            [this, id](int code, QProcess::ExitStatus status) {
                const bool ok = status == QProcess::NormalExit && code == 0;
                finishVideo(id, ok,
                            ok ? QString()
                               : QStringLiteral("yt-dlp exited with code %1").arg(code));
            });

    row->rec.status = DownloadStatus::Active;
    row->rec.error.clear();
    db_->updateDownloadStatus(id, DownloadStatus::Active);
    emit rowChanged(id);

    proc->start(ytdlp, args);
}

void DownloadManager::onVideoLine(qint64 id, const QString& line) {
    DownloadLiveRow* row = find(id);
    if (!row) return;

    if (line.startsWith("FDMPROG|")) {
        const QStringList p = line.split('|');
        if (p.size() < 5) return;
        auto num = [](const QString& s) -> qint64 {
            bool ok = false;
            const qint64 v = s.toLongLong(&ok);
            return ok ? v : -1;
        };
        const qint64 dl = num(p[1]);
        qint64 total = num(p[2]);
        if (total < 0) total = num(p[3]);  // fall back to the estimate
        bool sok = false;
        const double speed = p[4].toDouble(&sok);
        if (dl >= 0) row->bytesReceived = dl;
        if (total > 0) row->rec.totalBytes = total;
        row->bytesPerSec = sok ? speed : 0.0;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (row->rec.totalBytes > 0 &&
            now - lastPersistMs_.value(id, 0) >= kPersistThrottleMs) {
            lastPersistMs_[id] = now;
            db_->updateDownloadTotals(id, row->rec.totalBytes, false);
        }
        emit rowChanged(id);
        return;
    }

    // yt-dlp's `after_move:filepath` print: the finished file's absolute path.
    if (line.startsWith('/') &&
        QFileInfo(line).absolutePath() == QFileInfo(row->rec.outputPath).absolutePath()) {
        videoFinalPath_[id] = line;
    }
}

void DownloadManager::finishVideo(qint64 id, bool ok, const QString& error) {
    if (auto it = videoProcs_.find(id); it != videoProcs_.end()) {
        it.value()->deleteLater();
        videoProcs_.erase(it);
    }
    DownloadLiveRow* row = find(id);
    if (!row) {  // removed while running
        videoIntent_.remove(id);
        videoFinalPath_.remove(id);
        return;
    }
    row->bytesPerSec = 0;
    const QString intent = videoIntent_.take(id);

    if (intent == "cancel") {
        row->rec.status = DownloadStatus::Failed;
        row->rec.error = "Cancelled";
        db_->updateDownloadStatus(id, DownloadStatus::Failed, "Cancelled");
        videoFinalPath_.remove(id);
        emit rowChanged(id);
        return;
    }
    if (intent == "pause") {
        row->rec.status = DownloadStatus::Paused;
        db_->updateDownloadStatus(id, DownloadStatus::Paused);
        emit rowChanged(id);
        return;
    }
    if (ok) {
        const QString fp = videoFinalPath_.take(id);
        if (!fp.isEmpty() && fp != row->rec.outputPath) {
            row->rec.outputPath = fp;
            db_->updateDownloadOutputPath(id, fp);
        }
        if (row->rec.totalBytes <= 0 && row->bytesReceived > 0) {
            row->rec.totalBytes = row->bytesReceived;
        } else if (row->rec.totalBytes > 0) {
            row->bytesReceived = row->rec.totalBytes;
        }
        row->rec.status = DownloadStatus::Completed;
        row->rec.verification = VerificationStatus::Unverified;
        if (row->rec.totalBytes > 0) db_->updateDownloadTotals(id, row->rec.totalBytes, false);
        db_->updateDownloadStatus(id, DownloadStatus::Completed);
        db_->updateVerificationStatus(id, VerificationStatus::Unverified);
        emit rowChanged(id);
    } else {
        row->rec.status = DownloadStatus::Failed;
        row->rec.error = error.isEmpty() ? QStringLiteral("video download failed") : error;
        db_->updateDownloadStatus(id, DownloadStatus::Failed, row->rec.error);
        videoFinalPath_.remove(id);
        emit rowChanged(id);
    }
}

void DownloadManager::updateVideoBackend(std::function<void(bool, QString)> done) {
    const QString ytdlp = findHelper("yt-dlp");
    if (ytdlp.isEmpty()) {
        if (done) done(false, "yt-dlp not found");
        return;
    }
    auto* proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    auto fired = std::make_shared<bool>(false);
    auto finish = [proc, done, fired](bool ok, const QString& fallback) {
        if (*fired) return;  // finished() and errorOccurred() can both fire
        *fired = true;
        QString out = QString::fromUtf8(proc->readAll()).trimmed();
        if (out.isEmpty()) out = fallback;
        proc->deleteLater();
        if (done) done(ok, out);
    };
    connect(proc, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus st) {
        finish(st == QProcess::NormalExit && code == 0, QStringLiteral("Done."));
    });
    connect(proc, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError) {
        finish(false, QStringLiteral("Failed to run yt-dlp."));
    });
    proc->start(ytdlp, {"-U"});
}

void DownloadManager::probe(const QString& url,
                            std::function<void(ProbeResult)> onResult) {
    probe(url, {}, std::move(onResult));
}

void DownloadManager::probe(const QString& url,
                            const QList<QPair<QString, QString>>& headers,
                            std::function<void(ProbeResult)> onResult) {
    engine_->probe(
        url.toStdString(), toEngineHeaders(headers),
        [this, cb = std::move(onResult)](fdm::ProbeResult pr) {
            ProbeResult ui;
            ui.ok = pr.ok;
            ui.error = QString::fromStdString(pr.error);
            ui.finalUrl = QString::fromStdString(pr.info.finalUrl);
            ui.suggestedFilename = QString::fromStdString(pr.info.suggestedFilename);
            ui.contentLength = pr.info.contentLength;
            ui.supportsRanges = pr.info.supportsRanges;
            // Hop back to the UI thread (= the thread this QObject lives on).
            QMetaObject::invokeMethod(
                this, [cb, ui]() { cb(ui); }, Qt::QueuedConnection);
        });
}

fdm::ResumeSpec DownloadManager::buildResumeSpec(const DownloadLiveRow& row) const {
    fdm::ResumeSpec spec;
    spec.url = row.rec.url.toStdString();
    spec.outputPath = row.rec.outputPath.toStdString();
    spec.totalBytes = row.rec.totalBytes;
    spec.supportsRanges = row.rec.supportsRanges;
    spec.headers = headersFromJson(row.rec.requestHeaders);
    spec.chunks.reserve(row.chunks.size());
    for (const ChunkRecord& c : row.chunks) {
        fdm::ChunkRestore r;
        r.index = c.index;
        r.startByte = c.startByte;
        r.endByte = c.endByte;
        r.bytesReceived = c.bytesReceived;
        r.attempts = c.attempts;
        r.status = fromPersistStatus(c.status);
        spec.chunks.push_back(r);
    }
    return spec;
}

void DownloadManager::pause(qint64 id) {
    DownloadLiveRow* row = find(id);
    if (!row) return;
    if (row->rec.kind == "video") {
        if (auto* p = videoProcs_.value(id, nullptr)) {
            videoIntent_[id] = "pause";
            p->terminate();  // yt-dlp leaves a .part; resume continues it
        }
        return;
    }
    if (row->engineId == 0) {
        // Not currently in the engine -- nothing to ask the engine to do.
        // If we somehow got here for an Active row, sync the DB to Paused.
        if (row->rec.status == DownloadStatus::Active) {
            row->rec.status = DownloadStatus::Paused;
            db_->updateDownloadStatus(id, DownloadStatus::Paused);
            emit rowChanged(id);
        }
        return;
    }
    engine_->pause(row->engineId);
}

void DownloadManager::resume(qint64 id) {
    DownloadLiveRow* row = find(id);
    if (!row) return;
    if (row->rec.kind == "video") {
        if (!videoProcs_.contains(id)) spawnVideo(id);  // re-run; continues .part
        return;
    }
    if (row->engineId != 0) {
        // It's still in the engine and paused: just resume in place.
        engine_->resume(row->engineId);
        row->rec.status = DownloadStatus::Active;
        db_->updateDownloadStatus(id, DownloadStatus::Active);
        emit rowChanged(id);
        return;
    }

    // Cross-restart resume: hand the engine the persisted chunk state.
    // If we have no chunks persisted (download never got past probe), fall
    // back to a fresh start which will probe and split again.
    if (row->chunks.isEmpty() || row->rec.totalBytes < 0) {
        row->rec.status = DownloadStatus::Active;
        row->rec.error.clear();
        db_->updateDownloadStatus(id, DownloadStatus::Active);
        emit rowChanged(id);
        row->engineId = engine_->start(
            row->rec.url.toStdString(), row->rec.outputPath.toStdString(),
            headersFromJson(row->rec.requestHeaders),
            [this, id](fdm::EngineEvent ev) {
                QMetaObject::invokeMethod(
                    this, [this, id, ev = std::move(ev)]() mutable {
                        onEngineEvent(id, std::move(ev));
                    },
                    Qt::QueuedConnection);
            });
        return;
    }

    fdm::ResumeSpec spec = buildResumeSpec(*row);
    row->rec.status = DownloadStatus::Active;
    row->rec.error.clear();
    db_->updateDownloadStatus(id, DownloadStatus::Active);
    emit rowChanged(id);
    row->engineId = engine_->resumeKnown(
        std::move(spec), [this, id](fdm::EngineEvent ev) {
            QMetaObject::invokeMethod(
                this, [this, id, ev = std::move(ev)]() mutable {
                    onEngineEvent(id, std::move(ev));
                },
                Qt::QueuedConnection);
        });
}

void DownloadManager::retry(qint64 id) {
    DownloadLiveRow* row = find(id);
    if (!row) return;
    if (row->rec.kind == "video") {
        if (videoProcs_.contains(id)) return;
        row->bytesReceived = 0;
        row->rec.error.clear();
        spawnVideo(id);
        return;
    }
    if (row->engineId != 0) {
        engine_->cancel(row->engineId);
        row->engineId = 0;
    }
    // Reset live state and DB metadata so the upcoming engine.start() begins
    // from a clean slate. The engine's preallocateFile uses O_TRUNC, so the
    // existing partial file is overwritten -- no manual file removal needed.
    row->chunks.clear();
    row->bytesReceived = 0;
    row->bytesPerSec = 0;
    row->rec.totalBytes = -1;
    row->rec.supportsRanges = false;
    row->rec.status = DownloadStatus::Active;
    row->rec.error.clear();

    db_->updateDownloadStatus(id, DownloadStatus::Active);
    db_->updateDownloadTotals(id, -1, false);
    db_->replaceChunks(id, {});
    lastPersistMs_.remove(id);
    emit rowChanged(id);

    row->engineId = engine_->start(
        row->rec.url.toStdString(), row->rec.outputPath.toStdString(),
        headersFromJson(row->rec.requestHeaders),
        [this, id](fdm::EngineEvent ev) {
            QMetaObject::invokeMethod(
                this, [this, id, ev = std::move(ev)]() mutable {
                    onEngineEvent(id, std::move(ev));
                },
                Qt::QueuedConnection);
        });
}

void DownloadManager::cancel(qint64 id) {
    DownloadLiveRow* row = find(id);
    if (!row) return;
    if (row->rec.kind == "video") {
        if (auto* p = videoProcs_.value(id, nullptr)) {
            videoIntent_[id] = "cancel";
            p->kill();  // finishVideo writes the Failed/"Cancelled" status
        } else {
            row->rec.status = DownloadStatus::Failed;
            row->rec.error = "Cancelled";
            db_->updateDownloadStatus(id, DownloadStatus::Failed, "Cancelled");
            emit rowChanged(id);
        }
        return;
    }
    if (row->engineId != 0) {
        engine_->cancel(row->engineId);
        row->engineId = 0;
    }
    row->rec.status = DownloadStatus::Failed;
    row->rec.error = "Cancelled";
    db_->updateDownloadStatus(id, DownloadStatus::Failed, "Cancelled");
    emit rowChanged(id);
}

void DownloadManager::remove(qint64 id, bool alsoRemoveFile) {
    DownloadLiveRow* row = find(id);
    if (!row) return;
    if (row->engineId != 0) {
        engine_->cancel(row->engineId);
        row->engineId = 0;
    }
    if (auto* p = videoProcs_.value(id, nullptr)) {
        // Suppress terminal status writes; finishVideo will find the row gone.
        videoIntent_[id] = "cancel";
        p->kill();
    }
    const QString filePath = row->rec.outputPath;
    db_->deleteDownload(id);
    rows_.remove(id);
    rowOrder_.removeAll(id);
    lastPersistMs_.remove(id);
    if (alsoRemoveFile && !filePath.isEmpty()) {
        QFile::remove(filePath);
    }
    emit rowRemoved(id);
}

void DownloadManager::onEngineEvent(qint64 id, fdm::EngineEvent ev) {
    DownloadLiveRow* row = find(id);
    if (!row) return;  // row was removed before this event landed

    std::visit(
        [&](auto&& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, fdm::Started>) {
                row->rec.totalBytes = e.contentLength;
                row->rec.supportsRanges = e.supportsRanges;
                row->chunks = chunksFromSpecs(e.chunks);
                row->bytesReceived = 0;
                // Persist totals + initial chunk layout in one swoop. The
                // Active status was set when the user started/resumed; we
                // don't churn it here.
                db_->updateDownloadTotals(id, e.contentLength, e.supportsRanges);
                db_->replaceChunks(id, row->chunks);
                lastPersistMs_[id] = QDateTime::currentMSecsSinceEpoch();
                // If the server volunteered a digest AND the user didn't
                // already supply one at startNew time, persist it. User
                // hashes always win over server-side discovery.
                if (row->rec.hashSource != "user" && !e.expectedHash.empty()) {
                    row->rec.expectedHash = QString::fromStdString(e.expectedHash);
                    row->rec.hashAlgorithm = QString::fromStdString(e.hashAlgorithm);
                    row->rec.hashSource = QString::fromStdString(e.hashSource);
                    if (row->rec.verification == VerificationStatus::None) {
                        row->rec.verification = VerificationStatus::Pending;
                    }
                    db_->updateExpectedHash(id, row->rec.expectedHash,
                                            row->rec.hashAlgorithm,
                                            row->rec.hashSource);
                }
                emit rowChanged(id);
            } else if constexpr (std::is_same_v<T, fdm::Progress>) {
                // Dynamic re-segmentation grows the segment set at runtime. When
                // the count changes, rewrite the whole chunk layout (insert the
                // new rows); otherwise just update byte counts in place.
                const bool layoutChanged =
                    row->chunks.size() != static_cast<int>(e.chunks.size());
                row->chunks = chunksFromProgress(e.chunks);
                row->bytesReceived = e.received;
                row->activeConnections = e.activeConnections;
                row->connectionLimit = e.connectionLimit;
                // EMA on the raw 100ms-window rate the engine reports.
                constexpr double kAlpha = 0.2;
                row->bytesPerSec = (row->bytesPerSec <= 0)
                                       ? e.bytesPerSec
                                       : kAlpha * e.bytesPerSec +
                                             (1.0 - kAlpha) * row->bytesPerSec;
                if (layoutChanged) {
                    db_->replaceChunks(id, row->chunks);
                    lastPersistMs_[id] = QDateTime::currentMSecsSinceEpoch();
                } else {
                    persistProgressThrottled(id);
                }
                emit rowChanged(id);
            } else if constexpr (std::is_same_v<T, fdm::Paused>) {
                row->rec.status = DownloadStatus::Paused;
                row->bytesPerSec = 0;
                db_->updateDownloadStatus(id, DownloadStatus::Paused);
                // Always flush on a state transition; throttling is only
                // for the high-frequency Progress path.
                if (!row->chunks.isEmpty()) {
                    db_->updateChunkProgress(id, row->chunks);
                }
                emit rowChanged(id);
            } else if constexpr (std::is_same_v<T, fdm::Finished>) {
                row->engineId = 0;
                row->bytesPerSec = 0;
                if (row->rec.totalBytes > 0) {
                    row->bytesReceived = row->rec.totalBytes;
                } else if (row->bytesReceived > 0) {
                    // Server never told us Content-Length up front (chunked
                    // transfer-encoding, or just a 200 without the header).
                    // We know the true size now: it's what we actually read.
                    // Backfill totalBytes so the row's Size column and the
                    // progress bar read correctly from here on.
                    row->rec.totalBytes = row->bytesReceived;
                    db_->updateDownloadTotals(id, row->bytesReceived,
                                              row->rec.supportsRanges);
                }
                // Persist final chunk state, then transition to Finalizing.
                // The actual Completed transition happens after hash check.
                if (!row->chunks.isEmpty()) {
                    db_->updateChunkProgress(id, row->chunks);
                }
                row->rec.status = DownloadStatus::Finalizing;
                db_->updateDownloadStatus(id, DownloadStatus::Finalizing);
                emit rowChanged(id);
                beginFinalize(id);
            } else if constexpr (std::is_same_v<T, fdm::Failed>) {
                row->engineId = 0;
                row->rec.status = DownloadStatus::Failed;
                row->rec.error = QString::fromStdString(e.reason);
                row->bytesPerSec = 0;
                db_->updateDownloadStatus(id, DownloadStatus::Failed, row->rec.error);
                if (!row->chunks.isEmpty()) {
                    db_->updateChunkProgress(id, row->chunks);
                }
                emit rowChanged(id);
            }
        },
        ev);
}

void DownloadManager::beginFinalize(qint64 id) {
    DownloadLiveRow* row = find(id);
    if (!row) return;

    // Three branches based on what we already know:
    //   (a) hash known (user input or HTTP headers) -> hash + compare
    //   (b) no hash known yet -> try sidecar, then either (a) or (c)
    //   (c) sidecar also empty -> mark Unverified and finish without compare
    if (!row->rec.expectedHash.isEmpty()) {
        startHashJob(id);
        return;
    }

    // Probe headers had nothing; try sidecar files. Use the saved file's
    // basename when looking up manifest-style sidecars so multi-entry
    // checksum files resolve to OUR line.
    auto* fetcher = new SidecarFetcher(this);
    const QString baseUrl = row->rec.url;  // post-redirect URL not retained on row; use original
    const QString filename = QFileInfo(row->rec.outputPath).fileName();
    QPointer<DownloadManager> guard = this;
    connect(fetcher, &SidecarFetcher::result, this,
            [this, id, fetcher, guard](QString hash, QString algorithm, QString source) {
                fetcher->deleteLater();
                if (!guard) return;
                DownloadLiveRow* r = find(id);
                if (!r) return;
                if (!hash.isEmpty()) {
                    r->rec.expectedHash = hash;
                    r->rec.hashAlgorithm = algorithm;
                    r->rec.hashSource = source;
                    if (r->rec.verification == VerificationStatus::None) {
                        r->rec.verification = VerificationStatus::Pending;
                    }
                    db_->updateExpectedHash(id, hash, algorithm, source);
                    emit rowChanged(id);
                    startHashJob(id);
                } else {
                    // No hash anywhere -- complete without verification.
                    r->rec.status = DownloadStatus::Completed;
                    r->rec.verification = VerificationStatus::Unverified;
                    db_->updateDownloadStatus(id, DownloadStatus::Completed);
                    db_->updateVerificationStatus(id, VerificationStatus::Unverified);
                    emit rowChanged(id);
                }
            });
    fetcher->fetch(baseUrl, filename);
}

void DownloadManager::startHashJob(qint64 id) {
    DownloadLiveRow* row = find(id);
    if (!row) return;

    row->rec.verification = VerificationStatus::Verifying;
    db_->updateVerificationStatus(id, VerificationStatus::Verifying);
    emit rowChanged(id);

    const QString path = row->rec.outputPath;
    const QString algName = row->rec.hashAlgorithm;
    const QCryptographicHash::Algorithm alg = qtAlgFor(algName);

    // QtConcurrent::run dispatches onto the global thread pool. The worker
    // returns a hex digest (or empty on read failure); we observe via a
    // QFutureWatcher whose finished() fires back on the manager's thread.
    auto* watcher = new QFutureWatcher<QString>(this);
    QFuture<QString> future = QtConcurrent::run([path, alg]() -> QString {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return {};
        QCryptographicHash h(alg);
        if (!h.addData(&f)) return {};
        return QString::fromLatin1(h.result().toHex());
    });
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, id, watcher]() {
                const QString actual = watcher->result();
                watcher->deleteLater();
                onHashJobDone(id, actual);
            });
    watcher->setFuture(future);
}

void DownloadManager::onHashJobDone(qint64 id, const QString& actualHash) {
    DownloadLiveRow* row = find(id);
    if (!row) return;

    row->rec.actualHash = actualHash;
    const QString expected = row->rec.expectedHash.toLower();
    const QString actualLower = actualHash.toLower();

    if (actualHash.isEmpty()) {
        // Hash compute failed (file gone, IO error). Treat as a mismatch
        // so the user sees something went wrong rather than silently
        // accepting an unverified file we claimed we'd verify.
        row->rec.status = DownloadStatus::Failed;
        row->rec.verification = VerificationStatus::Mismatch;
        row->rec.error = "Could not read file for hash verification";
    } else if (actualLower == expected) {
        row->rec.status = DownloadStatus::Completed;
        row->rec.verification = VerificationStatus::Verified;
    } else {
        row->rec.status = DownloadStatus::Failed;
        row->rec.verification = VerificationStatus::Mismatch;
        row->rec.error =
            QString("Hash mismatch (%1): expected %2, got %3")
                .arg(row->rec.hashAlgorithm, expected, actualLower);
    }

    db_->updateVerificationResult(id, actualHash, row->rec.verification);
    db_->updateDownloadStatus(id, row->rec.status, row->rec.error);
    emit rowChanged(id);
}

void DownloadManager::persistProgressThrottled(qint64 id) {
    DownloadLiveRow* row = find(id);
    if (!row || row->chunks.isEmpty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 last = lastPersistMs_.value(id, 0);
    if (now - last < kPersistThrottleMs) return;
    lastPersistMs_[id] = now;
    db_->updateChunkProgress(id, row->chunks);
}

}  // namespace fdm::store
