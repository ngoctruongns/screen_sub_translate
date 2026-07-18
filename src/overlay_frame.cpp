#include "overlay_frame.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QMouseEvent>

OverlayFrame::OverlayFrame(QWidget *parent) : QWidget(parent)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
}

void OverlayFrame::moveEvent(QMoveEvent *event)
{
    QWidget::moveEvent(event);
    emit geometryChanged();
}

void OverlayFrame::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    emit geometryChanged();
}

void OverlayFrame::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) {
        showContextMenu(event->globalPosition().toPoint());
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        activeResizeEdges_ = hitTestEdges(event->position().toPoint());
        initialGeometry_ = geometry();
        initialMouseGlobalPos_ = event->globalPosition().toPoint();

        if (activeResizeEdges_ != Qt::Edges()) {
            resizing_ = true;
        } else {
            dragging_ = true;
            dragOffset_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
        }
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void OverlayFrame::mouseMoveEvent(QMouseEvent *event)
{
    if (resizing_ && (event->buttons() & Qt::LeftButton)) {
        applyResize(event->globalPosition().toPoint());
        event->accept();
        return;
    }

    if (dragging_ && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - dragOffset_);
        event->accept();
        return;
    }

    updateCursorForPosition(event->position().toPoint());
    QWidget::mouseMoveEvent(event);
}

void OverlayFrame::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        dragging_ = false;
        resizing_ = false;
        activeResizeEdges_ = Qt::Edges();
        unsetCursor();
        event->accept();
        emit geometryChanged();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void OverlayFrame::showContextMenu(const QPoint &globalPos)
{
    QMenu menu(this);
    buildContextMenu(menu);
    if (!menu.isEmpty()) {
        menu.addSeparator();
    }
    QAction *quitAction = menu.addAction(QStringLiteral("Quit"));
    QAction *picked = menu.exec(globalPos);
    if (picked == quitAction) {
        QApplication::quit();
    }
}

Qt::Edges OverlayFrame::hitTestEdges(const QPoint &localPos) const
{
    Qt::Edges edges;
    if (localPos.x() <= resizeMarginPx_) {
        edges |= Qt::LeftEdge;
    }
    if (localPos.x() >= width() - resizeMarginPx_) {
        edges |= Qt::RightEdge;
    }
    if (localPos.y() <= resizeMarginPx_) {
        edges |= Qt::TopEdge;
    }
    if (localPos.y() >= height() - resizeMarginPx_) {
        edges |= Qt::BottomEdge;
    }
    return edges;
}

void OverlayFrame::updateCursorForPosition(const QPoint &localPos)
{
    const Qt::Edges edges = hitTestEdges(localPos);
    if ((edges & Qt::TopEdge && edges & Qt::LeftEdge) ||
        (edges & Qt::BottomEdge && edges & Qt::RightEdge)) {
        setCursor(Qt::SizeFDiagCursor);
    } else if ((edges & Qt::TopEdge && edges & Qt::RightEdge) ||
               (edges & Qt::BottomEdge && edges & Qt::LeftEdge)) {
        setCursor(Qt::SizeBDiagCursor);
    } else if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
        setCursor(Qt::SizeHorCursor);
    } else if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
        setCursor(Qt::SizeVerCursor);
    } else {
        unsetCursor();
    }
}

void OverlayFrame::applyResize(const QPoint &globalPos)
{
    QRect next = initialGeometry_;
    const QPoint delta = globalPos - initialMouseGlobalPos_;

    if (activeResizeEdges_ & Qt::LeftEdge) {
        next.setLeft(next.left() + delta.x());
    }
    if (activeResizeEdges_ & Qt::RightEdge) {
        next.setRight(next.right() + delta.x());
    }
    if (activeResizeEdges_ & Qt::TopEdge) {
        next.setTop(next.top() + delta.y());
    }
    if (activeResizeEdges_ & Qt::BottomEdge) {
        next.setBottom(next.bottom() + delta.y());
    }

    if (next.width() < minimumWidth()) {
        if (activeResizeEdges_ & Qt::LeftEdge) {
            next.setLeft(next.right() - minimumWidth());
        } else {
            next.setWidth(minimumWidth());
        }
    }

    if (next.height() < minimumHeight()) {
        if (activeResizeEdges_ & Qt::TopEdge) {
            next.setTop(next.bottom() - minimumHeight());
        } else {
            next.setHeight(minimumHeight());
        }
    }

    setGeometry(next);
}
