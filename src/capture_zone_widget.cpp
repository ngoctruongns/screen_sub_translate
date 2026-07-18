#include "capture_zone_widget.h"

#include <algorithm>

#include <QEnterEvent>
#include <QLineF>
#include <QPainter>

CaptureZoneWidget::CaptureZoneWidget(QWidget *parent) : OverlayFrame(parent)
{
    setMinimumSize(120, 40);
}

QRect CaptureZoneWidget::captureRect() const
{
    return geometry().adjusted(kBorderThickness, kBorderThickness,
                               -kBorderThickness, -kBorderThickness);
}

void CaptureZoneWidget::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Draw only faint corner brackets and edge-midpoint ticks so the marker barely
    // overlaps the movie. Everything stays inside the outer margin band, i.e. outside
    // captureRect(), so it never contaminates the screen grab.
    const int alpha = hovered_ ? 170 : 70;
    QPen pen(QColor(255, 200, 140, alpha), 1, Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const QRectF r = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
    const qreal armX = std::min<qreal>(22.0, r.width() / 3.0);
    const qreal armY = std::min<qreal>(22.0, r.height() / 3.0);

    // Four corner brackets.
    painter.drawLine(QLineF(r.left(), r.top(), r.left() + armX, r.top()));
    painter.drawLine(QLineF(r.left(), r.top(), r.left(), r.top() + armY));
    painter.drawLine(QLineF(r.right(), r.top(), r.right() - armX, r.top()));
    painter.drawLine(QLineF(r.right(), r.top(), r.right(), r.top() + armY));
    painter.drawLine(QLineF(r.left(), r.bottom(), r.left() + armX, r.bottom()));
    painter.drawLine(QLineF(r.left(), r.bottom(), r.left(), r.bottom() - armY));
    painter.drawLine(QLineF(r.right(), r.bottom(), r.right() - armX, r.bottom()));
    painter.drawLine(QLineF(r.right(), r.bottom(), r.right(), r.bottom() - armY));

    // Short ticks at the midpoint of each edge.
    const qreal tick = 10.0;
    const qreal cx = r.center().x();
    const qreal cy = r.center().y();
    painter.drawLine(QLineF(cx - tick / 2, r.top(), cx + tick / 2, r.top()));
    painter.drawLine(QLineF(cx - tick / 2, r.bottom(), cx + tick / 2, r.bottom()));
    painter.drawLine(QLineF(r.left(), cy - tick / 2, r.left(), cy + tick / 2));
    painter.drawLine(QLineF(r.right(), cy - tick / 2, r.right(), cy + tick / 2));
}

void CaptureZoneWidget::enterEvent(QEnterEvent *event)
{
    hovered_ = true;
    update();
    OverlayFrame::enterEvent(event);
}

void CaptureZoneWidget::leaveEvent(QEvent *event)
{
    hovered_ = false;
    update();
    OverlayFrame::leaveEvent(event);
}
