#include "Icons.h"

#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>

namespace fdm_gui::icons {

namespace {

// Draw one glyph into the unit box [0,1]x[0,1]. The painter arrives scaled
// so 1.0 == icon edge; stroke width is pre-set on the pen.
void drawGlyph(QPainter& p, Glyph g) {
    switch (g) {
        case Glyph::Add:
            p.drawLine(QPointF(0.5, 0.15), QPointF(0.5, 0.85));
            p.drawLine(QPointF(0.15, 0.5), QPointF(0.85, 0.5));
            break;
        case Glyph::Pause:
            p.drawLine(QPointF(0.33, 0.18), QPointF(0.33, 0.82));
            p.drawLine(QPointF(0.67, 0.18), QPointF(0.67, 0.82));
            break;
        case Glyph::Play: {
            QPainterPath path;
            path.moveTo(0.28, 0.15);
            path.lineTo(0.85, 0.5);
            path.lineTo(0.28, 0.85);
            path.closeSubpath();
            p.setBrush(p.pen().color());
            p.drawPath(path);
            p.setBrush(Qt::NoBrush);
            break;
        }
        case Glyph::Cancel:
            p.drawLine(QPointF(0.2, 0.2), QPointF(0.8, 0.8));
            p.drawLine(QPointF(0.8, 0.2), QPointF(0.2, 0.8));
            break;
        case Glyph::Retry: {
            // Open circle with an arrowhead at the gap.
            p.drawArc(QRectF(0.15, 0.15, 0.7, 0.7), 16 * 60, 16 * 300);
            QPainterPath head;
            head.moveTo(0.78, 0.10);
            head.lineTo(0.92, 0.32);
            head.lineTo(0.66, 0.34);
            head.closeSubpath();
            p.setBrush(p.pen().color());
            p.drawPath(head);
            p.setBrush(Qt::NoBrush);
            break;
        }
        case Glyph::Redownload:
            // Down arrow over a tray.
            p.drawLine(QPointF(0.5, 0.12), QPointF(0.5, 0.58));
            p.drawLine(QPointF(0.3, 0.4), QPointF(0.5, 0.6));
            p.drawLine(QPointF(0.7, 0.4), QPointF(0.5, 0.6));
            p.drawPolyline(QPolygonF() << QPointF(0.15, 0.65) << QPointF(0.15, 0.85)
                                       << QPointF(0.85, 0.85) << QPointF(0.85, 0.65));
            break;
        case Glyph::Trash:
            p.drawLine(QPointF(0.15, 0.25), QPointF(0.85, 0.25));
            p.drawLine(QPointF(0.38, 0.25), QPointF(0.38, 0.14));
            p.drawLine(QPointF(0.62, 0.25), QPointF(0.62, 0.14));
            p.drawLine(QPointF(0.38, 0.14), QPointF(0.62, 0.14));
            p.drawPolyline(QPolygonF() << QPointF(0.23, 0.25) << QPointF(0.28, 0.88)
                                       << QPointF(0.72, 0.88) << QPointF(0.77, 0.25));
            p.drawLine(QPointF(0.42, 0.4), QPointF(0.43, 0.74));
            p.drawLine(QPointF(0.58, 0.4), QPointF(0.57, 0.74));
            break;
        case Glyph::Folder: {
            QPainterPath path;
            path.moveTo(0.12, 0.82);
            path.lineTo(0.12, 0.22);
            path.lineTo(0.4, 0.22);
            path.lineTo(0.5, 0.34);
            path.lineTo(0.88, 0.34);
            path.lineTo(0.88, 0.82);
            path.closeSubpath();
            p.drawPath(path);
            break;
        }
        case Glyph::Details:
            p.drawEllipse(QRectF(0.12, 0.12, 0.76, 0.76));
            p.drawLine(QPointF(0.5, 0.45), QPointF(0.5, 0.7));
            p.drawPoint(QPointF(0.5, 0.3));
            break;
        case Glyph::Dots:
            p.setBrush(p.pen().color());
            p.drawEllipse(QPointF(0.5, 0.2), 0.07, 0.07);
            p.drawEllipse(QPointF(0.5, 0.5), 0.07, 0.07);
            p.drawEllipse(QPointF(0.5, 0.8), 0.07, 0.07);
            p.setBrush(Qt::NoBrush);
            break;
        case Glyph::File: {
            QPainterPath path;
            path.moveTo(0.22, 0.1);
            path.lineTo(0.62, 0.1);
            path.lineTo(0.78, 0.28);
            path.lineTo(0.78, 0.9);
            path.lineTo(0.22, 0.9);
            path.closeSubpath();
            p.drawPath(path);
            p.drawLine(QPointF(0.62, 0.1), QPointF(0.62, 0.28));
            p.drawLine(QPointF(0.62, 0.28), QPointF(0.78, 0.28));
            break;
        }
        case Glyph::Video: {
            p.drawRoundedRect(QRectF(0.1, 0.18, 0.8, 0.64), 0.08, 0.08);
            QPainterPath tri;
            tri.moveTo(0.42, 0.36);
            tri.lineTo(0.64, 0.5);
            tri.lineTo(0.42, 0.64);
            tri.closeSubpath();
            p.setBrush(p.pen().color());
            p.drawPath(tri);
            p.setBrush(Qt::NoBrush);
            break;
        }
    }
}

}  // namespace

QIcon icon(Glyph glyph, const QColor& color) {
    const QColor c =
        color.isValid() ? color : QGuiApplication::palette().color(QPalette::Text);

    // Cache: same glyph+color is requested by every row paint.
    static QHash<quint64, QIcon> cache;
    const quint64 key = (quint64(c.rgba()) << 8) | quint64(glyph);
    auto it = cache.find(key);
    if (it != cache.end()) return *it;

    QIcon result;
    for (const int size : {16, 20, 24, 32, 48, 64}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(size, size);
        QPen pen(c);
        pen.setWidthF(0.09);  // stroke relative to icon edge
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        drawGlyph(p, glyph);
        result.addPixmap(pm);
    }
    cache.insert(key, result);
    return result;
}

}  // namespace fdm_gui::icons
