#pragma once

#include <QString>

#include "overlay_frame.h"

class QLabel;
class QResizeEvent;

// Independent, resizable window that displays the translated subtitle. The
// bubble fills the whole window; text word-wraps within the user-chosen size
// instead of auto-sizing to the text length.
class TranslationWidget : public OverlayFrame
{
    Q_OBJECT

public:
    explicit TranslationWidget(QWidget *parent = nullptr);

    void setText(const QString &text);
    QString text() const;
    void clear();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QLabel *label_ = nullptr;
    bool hovered_ = false;
};
