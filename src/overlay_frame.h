#pragma once

#include <QPoint>
#include <QRect>
#include <QWidget>

class QMenu;
class QMouseEvent;

// Base class for the independent, frameless, always-on-top overlay windows
// (the OCR capture zone and the translation display). Encapsulates the manual
// drag + edge-resize behaviour and a right-click context menu, since a
// frameless Qt::Tool window has no title bar to move or close it.
class OverlayFrame : public QWidget
{
    Q_OBJECT

public:
    explicit OverlayFrame(QWidget *parent = nullptr);
    ~OverlayFrame() override = default;

signals:
    // Emitted whenever the user finishes (or is in the middle of) moving or
    // resizing this window. The controller uses it to refresh the scan zone,
    // recompute the logical bounding box and persist the new geometry.
    void geometryChanged();

protected:
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

    // Subclasses may append their own actions to the right-click menu.
    virtual void buildContextMenu(QMenu &menu) { Q_UNUSED(menu); }

    int resizeMargin() const { return resizeMarginPx_; }

private:
    void showContextMenu(const QPoint &globalPos);
    Qt::Edges hitTestEdges(const QPoint &localPos) const;
    void updateCursorForPosition(const QPoint &localPos);
    void applyResize(const QPoint &globalPos);

    bool dragging_ = false;
    bool resizing_ = false;
    Qt::Edges activeResizeEdges_ = Qt::Edges();
    QRect initialGeometry_;
    QPoint initialMouseGlobalPos_;
    QPoint dragOffset_;
    const int resizeMarginPx_ = 10;
};
