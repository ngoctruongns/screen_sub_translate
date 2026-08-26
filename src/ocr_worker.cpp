#include "ocr_worker.h"

OcrWorker::OcrWorker(SourceLanguage language, QObject *parent)
    : QObject(parent), engine_(language)
{
}

void OcrWorker::processImage(const cv::Mat &processedImg, int requestId)
{
    if (!engine_.isReady()) {
        emit ocrError(QStringLiteral("ONNX OCR model not found or cannot initialize for %1")
                          .arg(sourcelang::displayName(engine_.language())),
                      requestId);
        return;
    }

    const OcrEngine::OcrResult result = engine_.performOcr(processedImg);
    emit ocrReady(result.text, result.confidence, requestId);
}

void OcrWorker::setLanguage(SourceLanguage language)
{
    const bool ok = engine_.setLanguage(language);
    emit languageChanged(language, ok);
}
