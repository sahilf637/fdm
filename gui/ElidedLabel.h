#pragma once

#include <QFontMetrics>
#include <QLabel>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QString>

namespace fdm_gui {

// A single-line label that elides its text with "…" to fit whatever width it is
// given, so a long value (e.g. a URL) can never widen its layout -- the display
// length stays bounded by the window, not the string. Hovering shows the full
// text. No Q_OBJECT: it only overrides virtuals, so it needs no meta-object.
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(QWidget* parent = nullptr) : QLabel(parent) {
        // Our sizeHint must not pin the layout to the full string width.
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    // Set the full (un-elided) text. Stored verbatim for the tooltip and
    // re-elided to the current width.
    void setFullText(const QString& text) {
        full_ = text;
        setToolTip(text);
        applyElision();
    }

protected:
    void resizeEvent(QResizeEvent* ev) override {
        QLabel::resizeEvent(ev);
        applyElision();  // re-fit on every width change
    }

private:
    void applyElision() {
        setText(fontMetrics().elidedText(full_, Qt::ElideRight, width()));
    }

    QString full_;
};

}  // namespace fdm_gui
