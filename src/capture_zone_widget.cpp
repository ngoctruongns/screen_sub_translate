#include "capture_zone_widget.h"

#include <QEnterEvent>
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

    // Fully transparent at rest so it never overlaps the movie. On hover, outline the
    // whole window so the user can see and grab it to reposition/resize. The border
    // stays inside the outer margin band (outside captureRect), so even while hovering
    // it is never captured by the screen grab.
    if (!hovered_) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 200, 140, 200), 1, Qt::DashLine));
    painter.drawRect(r);
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
