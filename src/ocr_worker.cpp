#include "ocr_worker.h"

OcrWorker::OcrWorker(QObject *parent) : QObject(parent)
{
}

void OcrWorker::processImage(const cv::Mat &processedImg, int requestId)
{
    if (!engine_.isReady()) {
        emit ocrError(QStringLiteral("ONNX OCR model not found or cannot initialize"), requestId);
        return;
    }

    const QString ocrText = engine_.performOcr(processedImg);
    emit ocrReady(ocrText, requestId);
}
