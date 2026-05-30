#include "SingleInstance.h"

#include <QLocalServer>
#include <QLocalSocket>

#include <memory>

#include "IpcProtocol.h"

namespace fdm_gui {

SingleInstance::SingleInstance(QObject* parent) : QObject(parent) {}

bool SingleInstance::sendToPrimary(const QByteArray& payload) {
    QLocalSocket sock;
    sock.connectToServer(fdm_ipc::serverName());
    if (!sock.waitForConnected(fdm_ipc::kIoTimeoutMs)) {
        return false;  // no primary running
    }
    fdm_ipc::writeMessage(sock, payload);
    // Best-effort: wait for the primary's ack so we don't exit before it has
    // read our payload. Ignore the contents.
    sock.waitForReadyRead(fdm_ipc::kIoTimeoutMs);
    sock.disconnectFromServer();
    return true;
}

bool SingleInstance::listen() {
    server_ = new QLocalServer(this);
    // A previous crash can leave a stale socket file that blocks listen().
    QLocalServer::removeServer(fdm_ipc::serverName());
    if (!server_->listen(fdm_ipc::serverName())) {
        return false;
    }
    connect(server_, &QLocalServer::newConnection, this,
            &SingleInstance::onNewConnection);
    return true;
}

void SingleInstance::onNewConnection() {
    while (QLocalSocket* sock = server_->nextPendingConnection()) {
        auto buffer = std::make_shared<QByteArray>();
        connect(sock, &QLocalSocket::readyRead, sock, [this, sock, buffer]() {
            buffer->append(sock->readAll());
            QByteArray payload;
            if (!fdm_ipc::tryReadMessage(*buffer, payload)) return;
            // Ack so the sender knows we took it, then tear the connection
            // down. Emit queued so the handler can run a nested (modal) event
            // loop after this socket has been cleaned up.
            fdm_ipc::writeMessage(*sock, QByteArrayLiteral("{\"ok\":true}"));
            sock->disconnectFromServer();
            sock->deleteLater();
            emit messageReceived(payload);
        });
        connect(sock, &QLocalSocket::disconnected, sock, &QLocalSocket::deleteLater);
    }
}

}  // namespace fdm_gui
