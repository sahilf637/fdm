#include <QApplication>
#include <curl/curl.h>

#include "MainWindow.h"

int main(int argc, char* argv[]) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    QApplication app(argc, argv);
    app.setApplicationName("Fresh Download Manager");
    app.setOrganizationName("fdm");

    fdm_gui::MainWindow w;
    w.show();

    const int rc = app.exec();
    curl_global_cleanup();
    return rc;
}
