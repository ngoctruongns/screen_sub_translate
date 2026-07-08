#pragma once

#include <QString>

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include <onnxruntime_cxx_api.h>

class OcrEngine
{
public:
    OcrEngine();
    ~OcrEngine();

    QString performOcr(const cv::Mat &inputImg);
    bool isReady() const;

private:
    QString decodeCtc(const float *logits, int timeSteps, int classes) const;
    bool loadCharset(const QString &charsetPath);
    static QString normalizeHanText(const QString &text);

    Ort::Env env_;
    Ort::SessionOptions sessionOptions_;
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
