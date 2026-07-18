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
    label_->setStyleSheet("QLabel {"
                          "background-color: rgba(0, 0, 0, 165);"
                          "color: rgb(0, 255, 100);"
                          "font-size: 30px;"
                          "font-weight: 700;"
                          "border-radius: 10px;"
                          "padding: 12px 16px;"
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
