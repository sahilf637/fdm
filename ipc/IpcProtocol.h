#pragma once

// Wire protocol shared by the GUI (QLocalServer, primary instance) and the
// native-messaging host (QLocalSocket client). A message is a single
// length-prefixed UTF-8 JSON blob: QDataStream's QByteArray (de)serialization
// already writes a quint32 length prefix followed by the bytes, so both ends
// stay in lockstep without hand-rolled framing.

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QLocalSocket>
#include <QString>

namespace fdm_ipc {

// Per-user local socket name. QLocalServer/QLocalSocket map this onto a socket
// under $XDG_RUNTIME_DIR on Linux. Keep in sync across GUI + host.
inline QString serverName() {
    return QStringLiteral("fdm-instance");
}

inline constexpr int kIoTimeoutMs = 3000;

// Serialize one framed message into a buffer ready to write to a socket.
inline QByteArray frame(const QByteArray& payload) {
    QByteArray buf;
    QDataStream out(&buf, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << payload;
    return buf;
}

// Write one framed message and block until it has been flushed. Returns false
// on write/flush failure.
inline bool writeMessage(QLocalSocket& sock, const QByteArray& payload) {
    const QByteArray buf = frame(payload);
    if (sock.write(buf) != buf.size()) return false;
    return sock.waitForBytesWritten(kIoTimeoutMs);
}

// Try to extract one complete framed message from `buffer` (which accumulates
// bytes across readyRead). On success sets `out`, drops the consumed bytes from
// `buffer`, and returns true. Returns false if a full frame isn't available
// yet (caller should read more and retry).
inline bool tryReadMessage(QByteArray& buffer, QByteArray& out) {
    if (buffer.size() < static_cast<int>(sizeof(quint32))) return false;
    quint32 len = 0;
    {
        QDataStream peek(buffer);
        peek.setVersion(QDataStream::Qt_6_0);
        peek >> len;
    }
    if (buffer.size() < static_cast<int>(sizeof(quint32) + len)) return false;
    QDataStream in(&buffer, QIODevice::ReadOnly);
    in.setVersion(QDataStream::Qt_6_0);
    in >> out;
    buffer.remove(0, static_cast<int>(sizeof(quint32) + len));
    return true;
}

}  // namespace fdm_ipc
