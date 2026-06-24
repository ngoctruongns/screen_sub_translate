#pragma once

#include <QObject>

#include <QString>

#include <opencv2/core.hpp>

#include "ocr_engine.h"

class OcrWorker : public QObject
{
    Q_OBJECT

public:
    explicit OcrWorker(QObject *parent = nullptr);

public slots:
    void processImage(const cv::Mat &processedImg, int requestId);

signals:
    void ocrReady(const QString &ocrText, int requestId);
    void ocrError(const QString &error, int requestId);

private:
    OcrEngine engine_;
};
