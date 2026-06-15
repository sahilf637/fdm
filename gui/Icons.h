#pragma once

#include <QColor>
#include <QIcon>

namespace fdm_gui::icons {

// Programmatically painted line icons (the system has no Qt SVG plugin, and
// QStyle's stock pixmaps look dated). All glyphs are drawn in a unit box and
// rendered at several pixel sizes, tinted with the given color.
enum class Glyph {
    Add,         // plus
    Pause,       // two bars
    Play,        // triangle
    Cancel,      // X
    Retry,       // circular arrow
    Redownload,  // arrow into tray
    Trash,       // bin
    Folder,
    Details,     // info circle
    Dots,        // vertical ellipsis
    File,        // generic document (fallback when no theme icon)
    Video,       // play inside a screen
};

// Build (and cache) the icon for a glyph. `color` defaults to the current
// palette's text color when invalid.
QIcon icon(Glyph glyph, const QColor& color = QColor());

}  // namespace fdm_gui::icons
