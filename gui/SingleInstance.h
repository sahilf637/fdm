#pragma once

#include <QObject>

class QLocalServer;

namespace fdm_gui {

// Single-instance guard + tiny IPC inbox built on QLocalServer/QLocalSocket.
//
// Startup contract:
//   1. Call sendToPrimary(payload). If it returns true, another instance is
//      already running and has received `payload` (a download request, or an
//      empty "raise" ping) -- this process should exit.
//   2. If it returns false, call listen() to become the primary. From then on
//      every framed message a secondary instance (or the native host) sends is
//      delivered via the messageReceived signal.
class SingleInstance : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(QObject* parent = nullptr);

    // Connect to an existing primary and hand it `payload` (may be empty to
    // just ask it to raise its window). Returns true if a primary answered.
    bool sendToPrimary(const QByteArray& payload);

    // Become the primary: remove any stale socket and start listening.
    // Returns false if listening failed.
    bool listen();

signals:
    // One complete framed payload received from a secondary instance / host.
    // Delivered as a queued connection target so handlers may safely open
    // modal dialogs.
    void messageReceived(const QByteArray& payload);

private:
    void onNewConnection();

    QLocalServer* server_ = nullptr;
};

}  // namespace fdm_gui
