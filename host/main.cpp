// fdm-native-host: the bridge between the browser extension and the FDM GUI.
//
// The browser speaks Chrome/Firefox "native messaging" over stdio: a 4-byte
// native-byte-order length prefix followed by a UTF-8 JSON message. We read one
// request, validate it, forward it to the running FDM instance over a local
// socket (launching FDM first if it isn't running), and write a small ack back
// to the browser.
//
// Cookies travel browser -> here (stdio) -> fdm-gui (local socket). We
// deliberately forward over the socket rather than via argv so session cookies
// never appear in `ps` / /proc/<pid>/cmdline.

#include <QByteArray>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <QUrl>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "IpcProtocol.h"

namespace {

// Chrome caps a single native message at 1 MiB; mirror that as a sanity bound.
constexpr quint32 kMaxMessage = 1u * 1024 * 1024;

// Read exactly n bytes from stdin. Returns false on EOF / error.
bool readExact(void* dst, std::size_t n) {
    auto* p = static_cast<char*>(dst);
    std::size_t got = 0;
    while (got < n) {
        const std::size_t r = std::fread(p + got, 1, n - got, stdin);
        if (r == 0) return false;
        got += r;
    }
    return true;
}

// Read one native-messaging frame from stdin into `out`. Returns false on a
// clean EOF (browser closed the port) or a malformed/oversized frame.
bool readNativeMessage(QByteArray& out) {
    std::uint32_t len = 0;
    if (!readExact(&len, sizeof(len))) return false;  // native byte order
    if (len == 0 || len > kMaxMessage) return false;
    out.resize(static_cast<int>(len));
    return readExact(out.data(), len);
}

// Write one native-messaging frame to stdout.
void writeNativeMessage(const QByteArray& payload) {
    const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
    std::fwrite(&len, sizeof(len), 1, stdout);
    std::fwrite(payload.constData(), 1, payload.size(), stdout);
    std::fflush(stdout);
}

void replyAndExit(bool ok, const QString& error, int code) {
    QJsonObject o;
    o.insert("ok", ok);
    if (!error.isEmpty()) o.insert("error", error);
    writeNativeMessage(QJsonDocument(o).toJson(QJsonDocument::Compact));
    std::exit(code);
}

// Reply with an arbitrary JSON object (used by the video probe, which returns a
// whole format list rather than just ok/error).
void replyObjectAndExit(const QJsonObject& o, int code) {
    writeNativeMessage(QJsonDocument(o).toJson(QJsonDocument::Compact));
    std::exit(code);
}

// Locate the fdm-gui command. The .sh wrapper is preferred (it strips
// snap-leaked LD_LIBRARY_PATH that breaks dynamic linking). We check both the
// install layout (GUI next to the host) and the build tree (host in build/host,
// GUI in build/gui), then fall back to PATH.
QString findGuiCommand() {
    const QString dir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        dir + "/fdm-gui.sh",         // installed next to host
        dir + "/fdm-gui",
        dir + "/../gui/fdm-gui.sh",  // build tree
        dir + "/../gui/fdm-gui",
    };
    for (const QString& c : candidates) {
        const QFileInfo fi(c);
        if (fi.exists() && fi.isExecutable()) return fi.absoluteFilePath();
    }
    return QStandardPaths::findExecutable("fdm-gui");  // empty if not found
}

// Connect to the running FDM instance, launching it if necessary. Returns a
// connected socket, or nullptr on failure.
QLocalSocket* connectToFdm() {
    auto trySock = []() -> QLocalSocket* {
        auto* s = new QLocalSocket();
        s->connectToServer(fdm_ipc::serverName());
        if (s->waitForConnected(fdm_ipc::kIoTimeoutMs)) return s;
        delete s;
        return nullptr;
    };

    if (QLocalSocket* s = trySock()) return s;

    // Not running: launch the GUI, then retry with backoff.
    const QString gui = findGuiCommand();
    if (!gui.isEmpty()) {
        QProcess p;
        p.setProgram(gui);
        // Detach the GUI's stdio from ours: we speak the native-messaging
        // protocol on stdout to the browser, so any handle the child inherits
        // would corrupt that stream and keep the browser's pipe open past our
        // own exit (the browser would think the host never finished).
        p.setStandardInputFile(QProcess::nullDevice());
        p.setStandardOutputFile(QProcess::nullDevice());
        p.setStandardErrorFile(QProcess::nullDevice());
        p.startDetached();
    }

    for (int attempt = 0; attempt < 32; ++attempt) {  // ~8s budget for cold start
        QThread::msleep(250);
        if (QLocalSocket* s = trySock()) return s;
    }
    return nullptr;
}

// ---------- video probe (yt-dlp) ----------

// Locate the yt-dlp binary: prefer one bundled next to us, else PATH.
QString findYtDlp() {
    const QString dir = QCoreApplication::applicationDirPath();
    for (const QString& c : {dir + "/yt-dlp", dir + "/yt-dlp.exe"}) {
        const QFileInfo fi(c);
        if (fi.exists() && fi.isExecutable()) return fi.absoluteFilePath();
    }
    return QStandardPaths::findExecutable("yt-dlp");
}

// Pull a byte size from a yt-dlp format/info object: exact filesize, else the
// estimate, else -1.
qint64 formatFilesize(const QJsonObject& f) {
    if (!f.value("filesize").isNull())
        return static_cast<qint64>(f.value("filesize").toDouble());
    if (!f.value("filesize_approx").isNull())
        return static_cast<qint64>(f.value("filesize_approx").toDouble());
    return -1;
}

// yt-dlp's protocol string names a segmented transport (HLS/DASH) that the
// engine's plain-range downloader can't handle.
bool isSegmentedProtocol(const QString& proto) {
    return proto.contains(QLatin1String("m3u8")) ||
           proto.contains(QLatin1String("dash"));
}

// Short human label for a format, e.g. "1080p60 video-only mp4", "audio m4a".
QString buildLabel(bool hasVideo, bool hasAudio, int height, double fps,
                   const QString& ext) {
    QString l;
    if (hasVideo) {
        l = height > 0 ? QString::number(height) + "p" : QStringLiteral("video");
        if (fps >= 50) l += QString::number(static_cast<int>(fps));  // 1080p60
        if (!hasAudio) l += " video-only";
    } else {
        l = QStringLiteral("audio");
    }
    if (!ext.isEmpty()) l += " " + ext;
    return l;
}

// Reduce yt-dlp's verbose `formats[]` to the fields the UI needs, dropping
// non-media entries (storyboards / metadata-only). `selector` is what the
// later download phase passes to `yt-dlp -f`.
QJsonArray curateFormats(const QJsonArray& formats) {
    QJsonArray out;
    for (const QJsonValue& v : formats) {
        const QJsonObject f = v.toObject();
        const QString vcodec = f.value("vcodec").toString();
        const QString acodec = f.value("acodec").toString();
        const bool hasVideo = !vcodec.isEmpty() && vcodec != QLatin1String("none");
        const bool hasAudio = !acodec.isEmpty() && acodec != QLatin1String("none");
        if (!hasVideo && !hasAudio) continue;

        const QString proto = f.value("protocol").toString();
        const bool segmented = isSegmentedProtocol(proto);
        const int height = f.value("height").isNull() ? 0 : f.value("height").toInt();
        const double fps = f.value("fps").isNull() ? 0.0 : f.value("fps").toDouble();
        const QString ext = f.value("ext").toString();

        const qint64 size = formatFilesize(f);

        const QString id = f.value("format_id").toString();
        QJsonObject o;
        o.insert("formatId", id);
        o.insert("label", buildLabel(hasVideo, hasAudio, height, fps, ext));
        o.insert("ext", ext);
        o.insert("hasVideo", hasVideo);
        o.insert("hasAudio", hasAudio);
        o.insert("needsAudioMerge", hasVideo && !hasAudio);
        o.insert("segmented", segmented);
        if (height > 0) o.insert("height", height);
        if (fps > 0) o.insert("fps", fps);
        o.insert("filesize", static_cast<double>(size));
        o.insert("vcodec", hasVideo ? vcodec : QString());
        o.insert("acodec", hasAudio ? acodec : QString());
        o.insert("note", f.value("format_note").toString());
        // Selector for the download phase. Use a height-based expression rather
        // than the exact itag: format ids aren't stable across yt-dlp
        // extractions (YouTube serves a different set each request), so a fixed
        // id often yields "Requested format is not available" at download time.
        // The /b fallbacks guarantee *something* is always picked.
        QString selector;
        if (hasVideo && height > 0) {
            selector = QStringLiteral("bv*[height<=%1]+ba/b[height<=%1]/b").arg(height);
        } else if (!hasVideo && hasAudio) {
            selector = QStringLiteral("ba/b");
        } else {
            selector = id;  // direct/progressive file without a known height
        }
        o.insert("selector", selector);
        out.append(o);
    }
    return out;
}

// Some extractors (notably the generic one for a direct .mp4/.webm URL) describe
// the single stream at the info-dict top level instead of in formats[]. Build
// one entry from it so a plain media file still shows up as downloadable.
QJsonObject singleFormatFromInfo(const QJsonObject& info) {
    const QString ext = info.value("ext").toString();
    const QString proto = info.value("protocol").toString();
    const int height = info.value("height").isNull() ? 0 : info.value("height").toInt();
    const double fps = info.value("fps").isNull() ? 0.0 : info.value("fps").toDouble();
    const qint64 size = formatFilesize(info);
    const QString id = info.value("format_id").toString();

    QJsonObject o;
    o.insert("formatId", id.isEmpty() ? QStringLiteral("0") : id);
    o.insert("label", buildLabel(true, true, height, fps, ext));
    o.insert("ext", ext);
    o.insert("hasVideo", true);   // a complete container -- treat as progressive
    o.insert("hasAudio", true);
    o.insert("needsAudioMerge", false);
    o.insert("segmented", isSegmentedProtocol(proto));
    if (height > 0) o.insert("height", height);
    if (fps > 0) o.insert("fps", fps);
    o.insert("filesize", static_cast<double>(size));
    o.insert("note", info.value("format_note").toString());
    o.insert("selector", id.isEmpty() ? QStringLiteral("best") : id);
    return o;
}

void failProbe(const QString& msg) {
    QJsonObject o;
    o.insert("ok", false);
    o.insert("error", msg);
    replyObjectAndExit(o, 1);
}

// Run `yt-dlp -J` for `url` and reply with { ok, title, formats[] }. Stateless:
// answers the browser directly without launching / touching the GUI.
void runProbeVideo(const QString& url, const QJsonObject& headers) {
    if (url.isEmpty()) failProbe("missing url");
    const QString ytdlp = findYtDlp();
    if (ytdlp.isEmpty()) failProbe("yt-dlp not found");

    QStringList args{"-J", "--no-warnings", "--no-playlist"};
    const QString ua = headers.value("User-Agent").toString();
    if (!ua.isEmpty()) args << "--user-agent" << ua;
    // Cookies are intentionally NOT forwarded: a logged-in YouTube session
    // forces the PO-token-gated web client, which returns no usable formats.
    args << "--" << url;

    QProcess proc;
    proc.start(ytdlp, args);
    if (!proc.waitForStarted(5000)) failProbe("could not start yt-dlp");
    if (!proc.waitForFinished(45000)) {
        proc.kill();
        failProbe("yt-dlp timed out");
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        const QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        failProbe(err.isEmpty() ? QStringLiteral("yt-dlp failed") : err.left(300));
    }
    const QJsonDocument doc =
        QJsonDocument::fromJson(proc.readAllStandardOutput().trimmed());
    if (!doc.isObject()) failProbe("unparseable yt-dlp output");

    const QJsonObject info = doc.object();
    QJsonArray formats = curateFormats(info.value("formats").toArray());
    // Generic/direct media URLs describe their single stream at the top level.
    if (formats.isEmpty() && !info.value("url").toString().isEmpty())
        formats.append(singleFormatFromInfo(info));

    QJsonObject reply;
    reply.insert("ok", true);
    reply.insert("title", info.value("title").toString());
    reply.insert("isLive", info.value("is_live").toBool());
    reply.insert("formats", formats);
    replyObjectAndExit(reply, 0);
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    QByteArray request;
    if (!readNativeMessage(request)) {
        replyAndExit(false, "no message", 1);
    }

    // Validate: must be a JSON object.
    const QJsonDocument doc = QJsonDocument::fromJson(request);
    if (!doc.isObject()) replyAndExit(false, "malformed request", 1);

    // Liveness check from the popup's "Test connection" button: answer without
    // touching FDM so it doesn't pop a download dialog.
    const QJsonObject obj = doc.object();
    if (obj.value("ping").toBool()) replyAndExit(true, QString(), 0);

    // Video probe: list downloadable formats/qualities via yt-dlp. Stateless --
    // answered here without involving the GUI. (runProbeVideo exits.)
    if (obj.value("type").toString() == "probe-video") {
        runProbeVideo(obj.value("url").toString(), obj.value("headers").toObject());
    }

    // Everything else (legacy file downloads and type=="download-video") is
    // forwarded to the GUI, which starts the appropriate kind of download.
    const QUrl url(obj.value("url").toString());
    if (url.scheme() != "http" && url.scheme() != "https") {
        replyAndExit(false, "unsupported url scheme", 1);
    }

    QLocalSocket* sock = connectToFdm();
    if (!sock) replyAndExit(false, "could not reach FDM", 1);

    if (!fdm_ipc::writeMessage(*sock, request)) {
        replyAndExit(false, "send failed", 1);
    }
    sock->waitForReadyRead(fdm_ipc::kIoTimeoutMs);  // wait for FDM's ack
    sock->disconnectFromServer();

    replyAndExit(true, QString(), 0);
}
