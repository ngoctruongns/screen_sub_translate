#pragma once

#include <QString>

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include <onnxruntime_cxx_api.h>

#include "source_language.h"
#include "tuning_params.h"

class OcrEngine
{
public:
    struct OcrResult
    {
        QString text;
        float confidence = 0.0f; // Mean per-character max-softmax probability (0..1).
    };

    explicit OcrEngine(SourceLanguage language = sourcelang::kDefault);
    ~OcrEngine();

    // Swap to the recognition model + charset of another source language. This rebuilds
    // the ONNX session, so it must run on the thread that owns the engine (the OCR
    // thread). On failure the engine is left un-initialized and isReady() returns false,
    // rather than silently recognizing with the wrong language's model.
    bool setLanguage(SourceLanguage language);
    SourceLanguage language() const { return language_; }

    OcrResult performOcr(const cv::Mat &inputImg);
    bool isReady() const;

private:
    bool loadModel(const tuning::LanguageProfile &profile);
    QString decodeCtc(const float *logits, int timeSteps, int classes, float *outConfidence) const;
    bool loadCharset(const QString &charsetPath);

    // Post-decode cleanup of a raw recognition. The two languages need genuinely
    // different rules, so they have separate implementations rather than one
    // parameterised filter:
    //   - Chinese: drop everything that is not Han / digit / subtitle punctuation and
    //     strip ALL whitespace (Han text has no word spacing; any space is OCR noise).
    //   - English: keep Latin letters, digits, apostrophes and hyphens, and PRESERVE
    //     single spaces — word boundaries carry meaning and cannot be discarded.
    QString normalizeRecognizedText(const QString &text) const;
    static QString normalizeHanText(const QString &text);
    static QString normalizeLatinText(const QString &text);

    SourceLanguage language_;
    const tuning::LanguageProfile *profile_;

    Ort::Env env_;
    std::unique_ptr<Ort::Session> session_;
    Ort::MemoryInfo memoryInfo_;

    std::string inputName_;
    std::string outputName_;
    std::vector<const char *> inputNames_;
    std::vector<const char *> outputNames_;
    std::vector<std::string> charset_;

    // Reused buffers to reduce per-frame allocations in performOcr.
    cv::Mat resizedBuffer_;
    cv::Mat canvasBuffer_;
    cv::Mat floatImgBuffer_;
    std::vector<float> inputTensorValues_;

    bool initialized_ = false;
};
