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
    if (doc.object().value("ping").toBool()) replyAndExit(true, QString(), 0);

    const QUrl url(doc.object().value("url").toString());
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
