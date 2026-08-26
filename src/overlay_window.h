#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QRect>
#include <QThread>
#include <QTimer>
#include <QQueue>

#include <opencv2/core.hpp>

#include "capture_worker.h"
#include "ocr_subtitle_filter.h"
#include "ocr_worker.h"
#include "source_language.h"
#include "subtitle_logger.h"
#include "translate_client.h"
#include "tuning_params.h"

class CaptureZoneWidget;
class TranslationWidget;

// Controller that owns the whole capture -> OCR -> translate -> display pipeline
// and the two independent overlay windows (the OCR capture zone and the
// translation display). It is not itself a visible widget; the "tool window" is
// only a logical bounding box of the two child windows.
class OverlayWindow : public QObject
{
    Q_OBJECT

public:
    explicit OverlayWindow(QObject *parent = nullptr);
    ~OverlayWindow() override;

private slots:
    void onImageProcessed(const cv::Mat &processedImg);
    void onOcrReady(const QString &ocrText, float confidence, int requestId);
    void onOcrError(const QString &error, int requestId);
    void onTranslationReady(const QString &translatedText, const QString &sourceText);
    void onTranslationError(const QString &error);
    void onOcrLanguageChanged(SourceLanguage language, bool ok);

private:
    void setupWidgets();
    // Switches the whole pipeline to another source language: reloads the OCR model on
    // the OCR thread, re-points the filter, translate client and subtitle logger, and
    // discards every piece of in-flight state carried over from the previous language.
    void setSourceLanguage(SourceLanguage language);
    SourceLanguage restoreSourceLanguage() const;
    void saveSourceLanguage() const;
    void resetPipelineState();
    void restoreWidgetGeometry();
    void saveWidgetGeometry() const;
    void onZoneGeometryChanged();
    void recomputeBoundingBox();
    void updateWorkerScanZone();
    void appendSubtitleLog(const QString &status, const QString &sourceText,
                           const QString &translatedText) const;
    void startSubtitleSegment(const QString &sourceText) const;
    void updateSubtitleSegmentTranslation(const QString &sourceText,
                                          const QString &translatedText) const;
    void endSubtitleSegment() const;
    void dispatchLatestOcr();
    void applyDefaultNoiseConfig();
    void handleDispatchCandidate(const OcrSubtitleFilter::Decision &decision, float confidence);
    void flushPendingIncompleteSubtitle();
    // Whether a stable read is a clause that should be held for its continuation. The
    // two languages have separate implementations: Chinese cuts mid-construction on a
    // small set of grammatical suffixes, English on trailing conjunctions/prepositions.
    static bool isIncompleteSubtitlePhrase(const QString &text, SourceLanguage language);
    static bool isIncompleteChinesePhrase(const QString &trimmed);
    static bool isIncompleteEnglishPhrase(const QString &trimmed);

    // Translation display queue
    struct TranslationEntry {
        QString translatedText;
        QString sourceText;
        qint64  enqueuedAtMs = 0;  // QDateTime::currentMSecsSinceEpoch()
    };
    void enqueueTranslation(const QString &translatedText, const QString &sourceText);
    void tickDisplayQueue();
    void showTranslationEntry(const TranslationEntry &entry);
    int  computeDisplayDurationMs(const QString &text) const;

    CaptureZoneWidget *captureZone_ = nullptr;
    TranslationWidget *translation_ = nullptr;
    QRect boundingBox_;

    QThread captureThread_;
    CaptureWorker *captureWorker_ = nullptr;
    QThread ocrThread_;
    OcrWorker *ocrWorker_ = nullptr;
    TranslateClient translateClient_;

    SourceLanguage sourceLanguage_ = sourcelang::kDefault;
    // Non-empty while the selected language has no usable OCR model. Kept on screen by
    // the display tick so a missing model is visible rather than silently blank.
    QString ocrDisabledMessage_;

    QString lastOcrText_;
    QString lastTranslation_;
    cv::Mat latestFrameForOcr_;
    int latestFrameRequestId_ = 0;
    int inFlightOcrRequestId_ = 0;
    bool ocrBusy_ = false;
    int minOcrLength_ = tuning::kMinOcrLength;
    int minCandidateStableMs_ = tuning::kMinCandidateStableMs;
    OcrSubtitleFilter ocrSubtitleFilter_;
    bool subtitleVisible_ = false;
    QElapsedTimer lastNonEmptySubtitleTimer_;
    QString pendingIncompleteSubtitle_;
    QElapsedTimer pendingIncompleteSubtitleTimer_;

    QRect lastSentScanZone_;

    QThread loggerThread_;
    SubtitleLogger *subtitleLogger_ = nullptr;

    // Translation display queue state
    QQueue<TranslationEntry> translationQueue_;
    QTimer                  *displayTimer_             = nullptr;
    QElapsedTimer            currentDisplayTimer_;
    int                      currentDisplayDurationMs_ = 0;
    bool                     displayingTranslation_    = false;
};
