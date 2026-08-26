#pragma once

#include <QRect>

#include "overlay_frame.h"
#include "source_language.h"

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

    // The source language shown as checked in the context menu. Set by the controller,
    // which owns the actual pipeline state; the widget only renders and reports choices.
    void setSourceLanguage(SourceLanguage language);
    SourceLanguage sourceLanguage() const { return sourceLanguage_; }

signals:
    // Emitted when the user picks a different source language from the context menu.
    void sourceLanguageSelected(SourceLanguage language);

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void buildContextMenu(QMenu &menu) override;

private:
    static constexpr int kBorderThickness = 4;
    bool hovered_ = false;
    SourceLanguage sourceLanguage_ = sourcelang::kDefault;
};
