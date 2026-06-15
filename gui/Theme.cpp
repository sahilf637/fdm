#include "Theme.h"

#include <QApplication>
#include <QPalette>
#include <QString>

#include "fdm/store/Database.h"

namespace fdm_gui::theme {

using fdm::store::DownloadStatus;

QColor accent() {
    return QColor(0x3b, 0x82, 0xf6);  // blue
}

bool isDark() {
    return QApplication::palette().color(QPalette::Window).lightness() < 128;
}

QColor statusColor(DownloadStatus status) {
    switch (status) {
        case DownloadStatus::Completed:  return QColor(0x2d, 0xa4, 0x4e);  // green
        case DownloadStatus::Failed:     return QColor(0xd1, 0x24, 0x2f);  // red
        case DownloadStatus::Paused:     return QColor(0xbf, 0x87, 0x00);  // amber
        case DownloadStatus::Queued:
        case DownloadStatus::Active:
        case DownloadStatus::Finalizing: return accent();
    }
    return accent();
}

void apply(QApplication& app) {
    const QString accentName = accent().name();
    const QString accentSoft =
        isDark() ? "rgba(59,130,246,0.22)" : "rgba(59,130,246,0.14)";
    const QString hoverSoft =
        isDark() ? "rgba(255,255,255,0.06)" : "rgba(0,0,0,0.05)";

    app.setStyleSheet(QString(R"(
QToolBar#topBar {
    background: palette(window);
    border: none;
    border-bottom: 1px solid palette(mid);
    padding: 6px 8px;
    spacing: 6px;
}
QToolBar#topBar QToolButton {
    background: transparent;
    border: none;
    border-radius: 6px;
    padding: 5px 10px;
}
QToolBar#topBar QToolButton:hover { background: %2; }
QToolBar#topBar QToolButton:pressed { background: %3; }
QToolBar#topBar QToolButton:disabled { color: palette(mid); }

QLineEdit#searchBox {
    background: palette(base);
    border: 1px solid palette(mid);
    border-radius: 14px;
    padding: 4px 12px;
}
QLineEdit#searchBox:focus { border-color: %1; }

QListWidget#sidebar {
    background: palette(window);
    border: none;
    border-right: 1px solid palette(mid);
    padding: 6px 4px;
    outline: none;
}
QListWidget#sidebar::item {
    border-radius: 6px;
    padding: 7px 10px;
    margin: 1px 2px;
}
QListWidget#sidebar::item:hover { background: %2; }
QListWidget#sidebar::item:selected {
    background: %3;
    color: %1;
}

QListView#downloadList {
    background: palette(base);
    border: none;
    outline: none;
}

QStatusBar {
    background: palette(window);
    border-top: 1px solid palette(mid);
}
QStatusBar QLabel { color: palette(text); padding-right: 6px; }

QMenu {
    background: palette(window);
    border: 1px solid palette(mid);
    padding: 4px;
}
QMenu::item {
    border-radius: 5px;
    padding: 5px 24px 5px 12px;
}
QMenu::item:selected { background: %3; color: %1; }
QMenu::separator {
    height: 1px;
    background: palette(mid);
    margin: 4px 8px;
}

QPushButton#primaryButton {
    background: %1;
    color: white;
    border: none;
    border-radius: 6px;
    padding: 6px 18px;
    font-weight: 600;
}
QPushButton#primaryButton:hover { background: #2f6fe0; }
QPushButton#primaryButton:pressed { background: #285fc2; }
QPushButton#primaryButton:disabled { background: palette(mid); }
)")
                          .arg(accentName, hoverSoft, accentSoft));
}

}  // namespace fdm_gui::theme
