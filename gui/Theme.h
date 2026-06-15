#pragma once

#include <QColor>

class QApplication;

namespace fdm::store {
enum class DownloadStatus;
}

namespace fdm_gui::theme {

// Accent used for selection, progress fill, and active-status text.
QColor accent();

// Status color for pills / progress fills (green completed, red failed,
// amber paused, accent for live states).
QColor statusColor(fdm::store::DownloadStatus status);

// True when the current palette is dark (used to pick contrast shades).
bool isDark();

// Install the app-wide stylesheet (sidebar, top bar, list, menus). Built on
// palette() color roles so it follows the system light/dark theme.
void apply(QApplication& app);

}  // namespace fdm_gui::theme
