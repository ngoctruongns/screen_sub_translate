#include "translation_widget.h"

#include <algorithm>

#include <QColorDialog>
#include <QEnterEvent>
#include <QFont>
#include <QFontMetrics>
#include <QGraphicsDropShadowEffect>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QSettings>

namespace
{
// Padding baked into the text panel geometry (the label stylesheet adds none, so
// the painted background hugs the text with exactly this margin).
constexpr int kPadH = 14;
constexpr int kPadV = 8;
constexpr int kEdgeGap = 4; // keep the panel off the resize edges

const QColor kDefaultTextColor(242, 232, 92); // pale yellow #F2E85C

struct ColorPreset {
    const char *name;
    QColor color;
};
const ColorPreset kColorPresets[] = {
    {"Pale yellow", QColor(242, 232, 92)},
    {"White", QColor(255, 255, 255)},
    {"Cream", QColor(255, 224, 163)},
    {"Soft green", QColor(124, 255, 176)},
};

constexpr int kDefaultFontSizePx = 30;
constexpr int kMinFontSizePx = 12;
constexpr int kMaxFontSizePx = 96;
const int kFontSizePresets[] = {22, 26, 30, 36, 42};
} // namespace

TranslationWidget::TranslationWidget(QWidget *parent) : OverlayFrame(parent)
{
    setMinimumSize(180, 46);

    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("overlay"));
        textColor_ = settings.value(QStringLiteral("translationTextColor"),
                                    kDefaultTextColor).value<QColor>();
        fontSizePx_ = settings.value(QStringLiteral("translationFontSizePx"),
                                     kDefaultFontSizePx).toInt();
        settings.endGroup();
    }
    if (!textColor_.isValid()) {
        textColor_ = kDefaultTextColor;
    }
    fontSizePx_ = std::clamp(fontSizePx_, kMinFontSizePx, kMaxFontSizePx);

    label_ = new QLabel(
        QStringLiteral("Right-click: options | Drag to move | Drag edges to resize"),
        this);
    label_->setAlignment(Qt::AlignCenter);
    label_->setWordWrap(true);
    // Let the parent frame receive the mouse so the whole window stays draggable
    // and resizable through the label.
    label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    applyLabelStyle();
    applyLabelFont();

    // Drop shadow keeps the text panel legible over bright scenes.
    auto *shadow = new QGraphicsDropShadowEffect(label_);
    shadow->setBlurRadius(8);
    shadow->setColor(QColor(0, 0, 0, 200));
    shadow->setOffset(0, 0);
    label_->setGraphicsEffect(shadow);

    updateLabelLayout();
}

void TranslationWidget::applyLabelStyle()
{
    if (!label_) {
        return;
    }
    // Transparent window; the panel behind the text is the label's own background,
    // sized to the text in updateLabelLayout(). Font size/weight live on the QFont
    // (see applyLabelFont) so QFontMetrics in updateLabelLayout stays accurate.
    label_->setStyleSheet(QStringLiteral("QLabel {"
                                         "background-color: rgba(20, 20, 20, 170);"
                                         "color: rgb(%1, %2, %3);"
                                         "border-radius: 8px;"
                                         "}")
                              .arg(textColor_.red())
                              .arg(textColor_.green())
                              .arg(textColor_.blue()));
}

void TranslationWidget::applyLabelFont()
{
    if (!label_) {
        return;
    }
    QFont f = label_->font();
    f.setPixelSize(fontSizePx_);
    f.setBold(true);
    label_->setFont(f);
}

void TranslationWidget::updateLabelLayout()
{
    if (!label_) {
        return;
    }

    const QString content = label_->text();
    if (content.isEmpty()) {
        label_->hide();
        return;
    }

    const int maxPanelW = std::max(40, width() - kEdgeGap * 2);
    const int maxTextW = std::max(20, maxPanelW - kPadH * 2);

    const QFontMetrics fm(label_->font());
    const QRect textRect = fm.boundingRect(QRect(0, 0, maxTextW, 100000),
                                           Qt::TextWordWrap | Qt::AlignCenter, content);

    const int panelW = std::clamp(textRect.width() + kPadH * 2, 40, maxPanelW);
    const int panelH = std::min(textRect.height() + kPadV * 2, height() - kEdgeGap * 2);

    const int x = (width() - panelW) / 2;
    const int y = (height() - panelH) / 2;

    label_->setGeometry(std::max(0, x), std::max(0, y), panelW, std::max(20, panelH));
    label_->show();
}

void TranslationWidget::setText(const QString &text)
{
    if (label_) {
        label_->setText(text);
        updateLabelLayout();
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
        updateLabelLayout();
    }
}

void TranslationWidget::setTextColor(const QColor &color)
{
    if (!color.isValid() || color == textColor_) {
        return;
    }
    textColor_ = color;
    applyLabelStyle();
    saveTextColor();
}

void TranslationWidget::saveTextColor() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("overlay"));
    settings.setValue(QStringLiteral("translationTextColor"), textColor_);
    settings.endGroup();
}

void TranslationWidget::setFontSize(int pixelSize)
{
    const int clamped = std::clamp(pixelSize, kMinFontSizePx, kMaxFontSizePx);
    if (clamped == fontSizePx_) {
        return;
    }
    fontSizePx_ = clamped;
    applyLabelFont();
    updateLabelLayout();
    saveFontSize();
}

void TranslationWidget::saveFontSize() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("overlay"));
    settings.setValue(QStringLiteral("translationFontSizePx"), fontSizePx_);
    settings.endGroup();
}

void TranslationWidget::resizeEvent(QResizeEvent *event)
{
    OverlayFrame::resizeEvent(event);
    updateLabelLayout();
}

void TranslationWidget::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);

    // The window is fully transparent at rest (the text panel is the label). Only
    // on hover do we outline the whole window so the user can find and grab it.
    if (!hovered_) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 255, 255, 180), 1, Qt::DashLine));
    painter.drawRoundedRect(r, 8.0, 8.0);
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

void TranslationWidget::buildContextMenu(QMenu &menu)
{
    QMenu *colorMenu = menu.addMenu(QStringLiteral("Text color"));

    for (const ColorPreset &preset : kColorPresets) {
        QAction *action = colorMenu->addAction(QString::fromUtf8(preset.name));
        action->setCheckable(true);
        action->setChecked(preset.color == textColor_);
        const QColor c = preset.color;
        connect(action, &QAction::triggered, this, [this, c]() { setTextColor(c); });
    }

    colorMenu->addSeparator();
    QAction *custom = colorMenu->addAction(QStringLiteral("Custom…"));
    connect(custom, &QAction::triggered, this, [this]() {
        const QColor picked =
            QColorDialog::getColor(textColor_, this, QStringLiteral("Subtitle text color"));
        if (picked.isValid()) {
            setTextColor(picked);
        }
    });

    QMenu *sizeMenu = menu.addMenu(QStringLiteral("Text size"));
    for (const int size : kFontSizePresets) {
        QAction *action = sizeMenu->addAction(QStringLiteral("%1 px").arg(size));
        action->setCheckable(true);
        action->setChecked(size == fontSizePx_);
        connect(action, &QAction::triggered, this, [this, size]() { setFontSize(size); });
    }
}
