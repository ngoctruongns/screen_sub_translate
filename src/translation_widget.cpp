#include "translation_widget.h"

#include <QLabel>

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
    // Transparent background: only the green text shows, so short or empty lines
    // don't cover the movie underneath.
    label_->setStyleSheet("QLabel {"
                          "background-color: transparent;"
                          "color: rgb(0, 255, 100);"
                          "font-size: 30px;"
                          "font-weight: 700;"
                          "padding: 6px 10px;"
                          "}");
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
