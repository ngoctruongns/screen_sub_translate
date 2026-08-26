#pragma once

#include <QObject>

#include <QString>

#include <opencv2/core.hpp>

#include "ocr_engine.h"
#include "source_language.h"

class OcrWorker : public QObject
{
    Q_OBJECT

public:
    explicit OcrWorker(SourceLanguage language = sourcelang::kDefault, QObject *parent = nullptr);

public slots:
    void processImage(const cv::Mat &processedImg, int requestId);
    // Reloads the recognition model for `language`. Runs on the OCR thread (invoke it
    // with a queued connection) because it rebuilds the ONNX session.
    void setLanguage(SourceLanguage language);

signals:
    void ocrReady(const QString &ocrText, float confidence, int requestId);
    void ocrError(const QString &error, int requestId);
    // Emitted after a setLanguage() call so the controller can surface a failed model
    // load (typically a missing .onnx / dictionary file for the selected language).
    void languageChanged(SourceLanguage language, bool ok);

private:
    OcrEngine engine_;
};
