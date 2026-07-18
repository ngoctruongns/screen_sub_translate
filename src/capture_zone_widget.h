#pragma once

#include <QRect>

#include "overlay_frame.h"

class QPaintEvent;

// Independent, resizable window that marks the OCR capture region. Its interior
// is fully transparent so the screen grab in CaptureWorker only sees the movie
// underneath; only a thin frame is painted in the outer margin, which sits
// *outside* captureRect() and therefore never contaminates the grabbed image.
class CaptureZoneWidget : public OverlayFrame
{
    Q_OBJECT

public:
    explicit CaptureZoneWidget(QWidget *parent = nullptr);

    // The screen-coordinate rectangle actually captured for OCR: the window
    // geometry shrunk by the painted border so no frame pixels are grabbed.
    QRect captureRect() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    static constexpr int kBorderThickness = 4;
    bool hovered_ = false;
};
