#pragma once

#include <QElapsedTimer>
#include <QPoint>
#include <QThread>
#include <QWidget>

#include <opencv2/core.hpp>

#include "capture_worker.h"
#include "ocr_worker.h"
#include "translate_client.h"
#include "tuning_params.h"

class QLabel;
class QMoveEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;

class OverlayWindow : public QWidget
{
    Q_OBJECT

private:
    enum class NoiseProfile { Fast, Balanced, Clean };

    enum class ResultPosition { AboveSource, BelowSource };

public:
    explicit OverlayWindow(QWidget *parent = nullptr);
    ~OverlayWindow() override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private slots:
    void onImageProcessed(const cv::Mat &processedImg);
    void onOcrReady(const QString &ocrText, int requestId);
    void onOcrError(const QString &error, int requestId);
    void onTranslationReady(const QString &translatedText, const QString &sourceText);
    void onTranslationError(const QString &error);

private:
    void setupUi();
    void setupHotkeys();
    void updateWorkerScanZone();
    void updateSubtitleLayout();
    QRect localScanZoneRect() const;
    void showPositionMenu(const QPoint &globalPos);
    void appendSubtitleLog(const QString &status, const QString &sourceText,
                           const QString &translatedText) const;
    void dispatchLatestOcr();
    void applyNoiseProfile(NoiseProfile profile);
    QString noiseProfileName(NoiseProfile profile) const;
    QString subtitleKey(const QString &text) const;
    bool isLikelySameSubtitle(const QString &left, const QString &right) const;
    bool shouldDispatchSubtitle(const QString &ocrText);
    Qt::Edges hitTestEdges(const QPoint &localPos) const;
    void updateCursorForPosition(const QPoint &localPos);
    void applyResize(const QPoint &globalPos);
    QRect computeCaptureZone() const;

    QLabel *subtitleLabel_ = nullptr;
    QThread captureThread_;
    CaptureWorker *captureWorker_ = nullptr;
    QThread ocrThread_;
    OcrWorker *ocrWorker_ = nullptr;
    TranslateClient translateClient_;

    QString lastOcrText_;
    QString lastTranslation_;
    QString lastCandidateOcr_;
    int candidateRepeatCount_ = 0;
    QElapsedTimer ocrAcceptedTimer_;
    cv::Mat latestFrameForOcr_;
    int latestFrameRequestId_ = 0;
    int inFlightOcrRequestId_ = 0;
    bool ocrBusy_ = false;
    bool dragging_ = false;
    bool resizing_ = false;
    Qt::Edges activeResizeEdges_ = Qt::Edges();
    QRect initialGeometry_;
    QPoint initialMouseGlobalPos_;
    QPoint dragOffset_;
    QString logFilePath_;
    NoiseProfile noiseProfile_ = NoiseProfile::Balanced;
    ResultPosition resultPosition_ = ResultPosition::AboveSource;
    const int resizeMarginPx_ = 10;
    int minOcrLength_ = tuning::kDefaultProfile.minOcrLength;
    int requiredRepeatCount_ = tuning::kDefaultProfile.requiredRepeatCount;
    int minOcrAcceptGapMs_ = tuning::kDefaultProfile.minOcrAcceptGapMs;
    int emptyOcrStreak_ = 0;
    QString lastDispatchedSubtitleKey_;
    QElapsedTimer subtitleDispatchTimer_;
    bool subtitleVisible_ = false;
};
