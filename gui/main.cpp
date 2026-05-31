#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QIcon>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <curl/curl.h>

#include <memory>

#include "MainWindow.h"
#include "SingleInstance.h"
#include "fdm/store/Database.h"
#include "fdm/store/DownloadManager.h"

namespace {

QString dbPath() {
    // AppDataLocation nests under both org and app name (~/.local/share/fdm/fdm)
    // which is ugly. GenericDataLocation gives us ~/.local/share and we append
    // a single "fdm" folder ourselves.
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
        "/fdm";
    QDir().mkpath(dir);
    return QDir(dir).filePath("downloads.db");
}

// Extract a download request from the command line. Supported form:
//   fdm-gui --add-download '<json>'
// where <json> matches the IPC payload schema (url/filename/dir/hash/headers).
// Returns the raw JSON bytes, or empty if no request was given. Note: argv is
// visible via `ps`, so the cookie-bearing path is the native host -> socket,
// not this; --add-download is mainly for manual testing.
QByteArray downloadRequestFromArgs(const QStringList& args) {
    const int i = args.indexOf("--add-download");
    if (i >= 0 && i + 1 < args.size()) {
        return args.at(i + 1).toUtf8();
    }
    return {};
}

}  // namespace

int main(int argc, char* argv[]) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    QApplication app(argc, argv);
    app.setApplicationName("fdm");
    app.setOrganizationName("fdm");
    // Lets GNOME/Wayland match this window to fdm.desktop (via WM_CLASS / app_id)
    // and show its Icon= in the dock + alt-tab. Pairs with install-desktop.sh.
    app.setDesktopFileName("fdm");

    // Window/taskbar icon (same mark as the browser extension). Multiple sizes
    // so it stays crisp from the title bar up to the task switcher.
    QIcon icon;
    for (const char* res : {":/icons/fdm-16.png", ":/icons/fdm-32.png",
                            ":/icons/fdm-48.png", ":/icons/fdm-128.png",
                            ":/icons/fdm-256.png"}) {
        icon.addFile(res);
    }
    app.setWindowIcon(icon);

    const QByteArray launchPayload = downloadRequestFromArgs(app.arguments());

    // Single-instance: if a primary is already running, hand it our request (or
    // an empty raise-ping) and exit. This is also how the native host injects
    // browser downloads into an already-open window.
    fdm_gui::SingleInstance instance;
    if (instance.sendToPrimary(launchPayload)) {
        curl_global_cleanup();
        return 0;
    }

    std::unique_ptr<fdm::store::Database> db;
    try {
        db = std::make_unique<fdm::store::Database>(dbPath());
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, "Database error",
                              QString("Failed to open download database:\n%1").arg(e.what()));
        curl_global_cleanup();
        return 1;
    }

    fdm::store::DownloadManager manager(db.get());
    fdm_gui::MainWindow w(&manager);

    // Become the primary and route every incoming IPC payload to the window.
    // Queued so handlers may run nested (modal) event loops safely.
    instance.listen();
    QObject::connect(&instance, &fdm_gui::SingleInstance::messageReceived, &w,
                     &fdm_gui::MainWindow::handleIpcMessage, Qt::QueuedConnection);

    w.show();

    // If we were launched with a request, handle it once the window is up.
    if (!launchPayload.isEmpty()) {
        QTimer::singleShot(0, &w, [&w, launchPayload]() {
            w.handleIpcMessage(launchPayload);
        });
    }

    const int rc = app.exec();
    curl_global_cleanup();
    return rc;
}
