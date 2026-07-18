#include "capture_zone_widget.h"

#include <QEnterEvent>
#include <QPainter>
#include <QPainterPath>

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

    // Fill only the outer border band (full rect minus the transparent interior),
    // so the captured region stays clean. OddEvenFill leaves the inner rect empty.
    const QRect inner = rect().adjusted(kBorderThickness, kBorderThickness,
                                        -kBorderThickness, -kBorderThickness);
    QPainterPath ring;
    ring.setFillRule(Qt::OddEvenFill);
    ring.addRect(rect());
    ring.addRect(inner);

    const int alpha = hovered_ ? 200 : 90;
    painter.fillPath(ring, QColor(255, 180, 0, alpha));
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
