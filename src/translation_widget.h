#pragma once

#include <QColor>
#include <QString>

#include "overlay_frame.h"

class QLabel;
class QMenu;
class QResizeEvent;

// Independent, resizable window that displays the translated subtitle. The whole
// window is transparent; only a small panel sized to the current text is drawn
// behind it for contrast, so empty or short lines don't cover the movie. The
// text colour is user-selectable and persisted.
class TranslationWidget : public OverlayFrame
{
    Q_OBJECT

public:
    explicit TranslationWidget(QWidget *parent = nullptr);

    void setText(const QString &text);
    QString text() const;
    void clear();

    void setTextColor(const QColor &color);
    QColor textColor() const { return textColor_; }

    void setFontSize(int pixelSize);
    int fontSize() const { return fontSizePx_; }

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void buildContextMenu(QMenu &menu) override;

private:
    void applyLabelStyle();
    void applyLabelFont();
    void updateLabelLayout();
    void saveTextColor() const;
    void saveFontSize() const;

    QLabel *label_ = nullptr;
    QColor textColor_;
    int fontSizePx_ = 30;
    bool hovered_ = false;
};
