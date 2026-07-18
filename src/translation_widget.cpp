#include "translation_widget.h"

#include <QEnterEvent>
#include <QGraphicsDropShadowEffect>
#include <QLabel>
#include <QPainter>

TranslationWidget::TranslationWidget(QWidget *parent) : OverlayFrame(parent)
{
    setMinimumSize(180, 46);

    label_ = new QLabel(
        QStringLiteral("Right-click: options | Drag to move | Drag edges to resize"),
        this);
    label_->setAlignment(Qt::AlignCenter);
    label_->setWordWrap(true);
    // Let the parent frame receive the mouse so the whole bubble stays draggable
    // and resizable through the label.
    label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    // Transparent label background: the faint translucent panel is painted by the
    // widget itself (see paintEvent), so short or empty lines barely cover the movie.
    label_->setStyleSheet("QLabel {"
                          "background-color: transparent;"
                          "color: rgb(0, 255, 100);"
                          "font-size: 30px;"
                          "font-weight: 700;"
                          "padding: 6px 10px;"
                          "}");

    // Drop shadow keeps the green text legible over bright scenes without a solid box.
    auto *shadow = new QGraphicsDropShadowEffect(label_);
    shadow->setBlurRadius(6);
    shadow->setColor(QColor(0, 0, 0, 220));
    shadow->setOffset(0, 0);
    label_->setGraphicsEffect(shadow);

    label_->setGeometry(rect());
}

void TranslationWidget::setText(const QString &text)
{
    if (label_) {
        label_->setText(text);
    }
}

QString TranslationWidget::text() const
{
    return label_ ? label_->text() : QString();
}

void TranslationWidget::clear()
{
    if (label_) {
        label_->clear();
    }
}

void TranslationWidget::resizeEvent(QResizeEvent *event)
{
    OverlayFrame::resizeEvent(event);
    if (label_) {
        label_->setGeometry(rect());
    }
}

void TranslationWidget::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Faint translucent panel + border so the user can see and grab the window to
    // resize it, while covering as little of the movie as possible. The panel gets
    // a little more opaque on hover to make the edges obvious.
    const QRectF r = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
    const int fillAlpha = hovered_ ? 90 : 45;
    const int borderAlpha = hovered_ ? 180 : 80;

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, fillAlpha));
    painter.drawRoundedRect(r, 10.0, 10.0);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 255, 255, borderAlpha), 1, Qt::DashLine));
    painter.drawRoundedRect(r, 10.0, 10.0);
}

void TranslationWidget::enterEvent(QEnterEvent *event)
{
    hovered_ = true;
    update();
    OverlayFrame::enterEvent(event);
}

void TranslationWidget::leaveEvent(QEvent *event)
{
    hovered_ = false;
    update();
    OverlayFrame::leaveEvent(event);
}
