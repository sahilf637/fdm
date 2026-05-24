#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>
#include <curl/curl.h>

#include <memory>

#include "MainWindow.h"
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

}  // namespace

int main(int argc, char* argv[]) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    QApplication app(argc, argv);
    app.setApplicationName("fdm");
    app.setOrganizationName("fdm");

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
    w.show();

    const int rc = app.exec();
    curl_global_cleanup();
    return rc;
}
