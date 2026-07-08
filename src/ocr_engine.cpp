#include "ocr_engine.h"

#include "tuning_params.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace
{
bool isHanCodepoint(const ushort u)
{
    return (u >= 0x3400 && u <= 0x9FFF) || (u >= 0xF900 && u <= 0xFAFF);
}

bool isAsciiDigit(const ushort u)
{
    return u >= 0x30 && u <= 0x39;
}

bool isCommonSubtitlePunctuation(const ushort u)
{
    // Keep punctuation often seen in subtitles (ASCII + CJK full-width variants).
    static const QString punctuation = QStringLiteral("!\"'(),-.:;?[]{}...…、。！（），：；？");
    return punctuation.contains(QChar(u));
}

QString stripWhitespace(const QString &input)
{
    QString out;
    out.reserve(input.size());
    for (const QChar c : input)
    {
        if (!c.isSpace())
        {
            out.append(c);
        }
    }
    return out;
}

std::string toUtf8(const QString &s)
{
    return s.toUtf8().toStdString();
}

QString resolveRuntimePath(const QString &rawPath)
{
    const QFileInfo directInfo(rawPath);
    if (directInfo.isAbsolute())
    {
        return directInfo.absoluteFilePath();
    }

    const QString currentResolved = QFileInfo(QDir::current(), rawPath).absoluteFilePath();
    if (QFileInfo::exists(currentResolved))
    {
        return currentResolved;
    }

    const QString appResolved = QFileInfo(QCoreApplication::applicationDirPath(), rawPath).absoluteFilePath();
    if (QFileInfo::exists(appResolved))
    {
        return appResolved;
    }

    return appResolved;
}

cv::Mat cropLikelySubtitleRegion(const cv::Mat &bgr)
{
    if (bgr.empty())
    {
        return {};
    }

    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, gray, cv::Size(3, 3), 0.0);

    cv::Mat bin;
    cv::threshold(gray, bin, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);

    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(bin, bin, cv::MORPH_CLOSE, kernel, cv::Point(-1, -1), 1);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(bin, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    const int imgW = bgr.cols;
    const int imgH = bgr.rows;
    const double minArea = static_cast<double>(imgW * imgH) * 0.0008;

    cv::Rect merged;
    bool hasMerged = false;
    for (const auto &contour : contours)
    {
        const cv::Rect r = cv::boundingRect(contour);
        const double area = static_cast<double>(r.area());
        if (area < minArea)
        {
            continue;
        }

        if (r.height < static_cast<int>(imgH * 0.05) || r.height > static_cast<int>(imgH * 0.75))
        {
            continue;
        }

        if (r.y + r.height < static_cast<int>(imgH * 0.20))
        {
            continue;
        }

        merged = hasMerged ? (merged | r) : r;
        hasMerged = true;
    }

    if (!hasMerged)
    {
        return bgr;
    }

    if (merged.width < static_cast<int>(imgW * 0.20) || merged.height < static_cast<int>(imgH * 0.08))
    {
        return bgr;
    }

    const int padX = std::max(4, static_cast<int>(imgW * 0.02));
    const int padY = std::max(2, static_cast<int>(imgH * 0.02));
    const int x = std::max(0, merged.x - padX);
    const int y = std::max(0, merged.y - padY);
    const int w = std::min(imgW - x, merged.width + padX * 2);
    const int h = std::min(imgH - y, merged.height + padY * 2);

    if (w <= 8 || h <= 8)
    {
        return bgr;
    }

    return bgr(cv::Rect(x, y, w, h)).clone();
}
} // namespace

OcrEngine::OcrEngine()
    : env_(ORT_LOGGING_LEVEL_WARNING, "screen_sub_translate"),
      memoryInfo_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
{
    try
    {
        sessionOptions_.SetIntraOpNumThreads(1);
        sessionOptions_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

        if (tuning::kUseCudaExecutionProvider)
        {
            try
            {
                OrtCUDAProviderOptions cudaOptions{};
                cudaOptions.device_id = 0;
                cudaOptions.arena_extend_strategy = 0;
                cudaOptions.gpu_mem_limit = std::numeric_limits<size_t>::max();
                cudaOptions.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchExhaustive;
                cudaOptions.do_copy_in_default_stream = 1;
                sessionOptions_.AppendExecutionProvider_CUDA(cudaOptions);
            }
            catch (const std::exception &e)
            {
                qWarning() << "CUDA EP unavailable, falling back to CPU:" << e.what();
            }
        }

        const QString modelPath = resolveRuntimePath(QString::fromUtf8(tuning::kPaddleRecOnnxPath));
        const QString charsetPath = resolveRuntimePath(QString::fromUtf8(tuning::kPaddleCharsetPath));

        if (!QFileInfo::exists(modelPath))
        {
            qWarning() << "Failed to load Paddle model file:" << modelPath;
            return;
        }

        if (!loadCharset(charsetPath))
        {
            qWarning() << "Failed to load Paddle charset file:" << charsetPath;
            return;
        }

        session_ = std::make_unique<Ort::Session>(env_, modelPath.toUtf8().constData(), sessionOptions_);

        Ort::AllocatorWithDefaultOptions allocator;
        {
            auto name = session_->GetInputNameAllocated(0, allocator);
            inputName_ = name.get();
        }
        {
            auto name = session_->GetOutputNameAllocated(0, allocator);
            outputName_ = name.get();
        }

        inputNames_.push_back(inputName_.c_str());
        outputNames_.push_back(outputName_.c_str());

        initialized_ = true;
    }
    catch (const std::exception &e)
    {
        qWarning() << "ONNX Runtime init failed:" << e.what();
        initialized_ = false;
    }
}

OcrEngine::~OcrEngine() = default;

bool OcrEngine::loadCharset(const QString &charsetPath)
{
    QFile file(charsetPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return false;
    }

    QTextStream in(&file);
    charset_.clear();
    charset_.push_back(""); // CTC blank at index 0.

    while (!in.atEnd())
    {
        const QString line = in.readLine().trimmed();
        if (!line.isEmpty())
        {
            charset_.push_back(toUtf8(line));
        }
    }

    return charset_.size() > 1;
}

QString OcrEngine::decodeCtc(const float *logits, int timeSteps, int classes) const
{
    if (logits == nullptr || timeSteps <= 0 || classes <= 0)
    {
        return QString();
    }

    std::string out;
    out.reserve(static_cast<size_t>(timeSteps) * 2);

    int prev = -1;
    for (int t = 0; t < timeSteps; ++t)
    {
        const float *row = logits + static_cast<size_t>(t) * classes;
        int bestIdx = 0;
        float bestVal = row[0];
        for (int c = 1; c < classes; ++c)
        {
            if (row[c] > bestVal)
            {
                bestVal = row[c];
                bestIdx = c;
            }
        }

        if (bestIdx != 0 && bestIdx != prev && bestIdx < static_cast<int>(charset_.size()))
        {
            out += charset_[bestIdx];
        }
        prev = bestIdx;
    }

    return QString::fromUtf8(out.c_str());
}

QString OcrEngine::normalizeHanText(const QString &text)
{
    QString t = text.trimmed();
    t = stripWhitespace(t);

    QString out;
    out.reserve(t.size());
    for (const QChar c : t)
    {
        const ushort u = c.unicode();
        if (isHanCodepoint(u) || isAsciiDigit(u) || isCommonSubtitlePunctuation(u))
        {
            out.append(c);
        }
    }
    return out;
}

QString OcrEngine::performOcr(const cv::Mat &inputImg)
{
    if (!initialized_ || inputImg.empty() || !session_)
    {
        return QString();
    }

    // Convert to BGR 3-channel (PP-OCRv4 rec expects [1,3,H,W]).
    cv::Mat bgr;
    if (inputImg.channels() == 1)
    {
        cv::cvtColor(inputImg, bgr, cv::COLOR_GRAY2BGR);
    }
    else if (inputImg.channels() == 3)
    {
        bgr = inputImg;
    }
    else if (inputImg.channels() == 4)
    {
        cv::cvtColor(inputImg, bgr, cv::COLOR_BGRA2BGR);
    }
    else
    {
        qWarning() << "Unsupported input channels for OCR:" << inputImg.channels();
        return QString();
    }

    // Keep text aspect ratio, then pad to model width to reduce CTC hallucinations.
    const int targetH = tuning::kPaddleInputHeight;
    const int targetW = tuning::kPaddleInputWidth;

    const cv::Mat subtitleRegion = cropLikelySubtitleRegion(bgr);

    const float regionRatio = static_cast<float>(subtitleRegion.cols) / std::max(1, subtitleRegion.rows);
    int resizedW = static_cast<int>(std::ceil(targetH * regionRatio));
    resizedW = std::clamp(resizedW, 8, targetW);

    resizedBuffer_.create(targetH, resizedW, CV_8UC3);
    cv::resize(subtitleRegion, resizedBuffer_, cv::Size(resizedW, targetH), 0, 0, cv::INTER_LINEAR);

    canvasBuffer_.create(targetH, targetW, CV_8UC3);
    canvasBuffer_.setTo(cv::Scalar(0, 0, 0));
    resizedBuffer_.copyTo(canvasBuffer_(cv::Rect(0, 0, resizedW, targetH)));

    // Normalize mean=0.5 std=0.5 and pack as planar NCHW float.
    floatImgBuffer_.create(targetH, targetW, CV_32FC3);
    canvasBuffer_.convertTo(floatImgBuffer_, CV_32FC3, 1.0 / 255.0);

    constexpr int C = 3;
    const size_t planeSize = static_cast<size_t>(tuning::kPaddleInputHeight) * tuning::kPaddleInputWidth;
    const size_t totalInputSize = static_cast<size_t>(C) * planeSize;
    if (inputTensorValues_.size() != totalInputSize)
    {
        inputTensorValues_.resize(totalInputSize);
    }

    // Split channels BGR -> planar [B, G, R], normalize (x - 0.5) / 0.5.
    const float *src = reinterpret_cast<const float *>(floatImgBuffer_.data);
    for (int c = 0; c < C; ++c)
    {
        float *dst = inputTensorValues_.data() + static_cast<size_t>(c) * planeSize;
        for (size_t i = 0; i < planeSize; ++i)
        {
            dst[i] = (src[i * C + c] - 0.5f) / 0.5f;
        }
    }

    const std::array<int64_t, 4> shape = {1, C, tuning::kPaddleInputHeight, tuning::kPaddleInputWidth};
    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
        memoryInfo_, inputTensorValues_.data(), inputTensorValues_.size(), shape.data(), shape.size());

    try
    {
        auto outputTensors = session_->Run(
            Ort::RunOptions{nullptr},
            inputNames_.data(),
            &inputTensor,
            1,
            outputNames_.data(),
            1);

        if (outputTensors.empty() || !outputTensors[0].IsTensor())
        {
            return QString();
        }

        const Ort::TensorTypeAndShapeInfo info = outputTensors[0].GetTensorTypeAndShapeInfo();
        const std::vector<int64_t> dims = info.GetShape();
        if (dims.size() < 3)
        {
            return QString();
        }

        const int timeSteps = static_cast<int>(dims[dims.size() - 2]);
        const int classes = static_cast<int>(dims[dims.size() - 1]);
        const float *logits = outputTensors[0].GetTensorData<float>();

        return normalizeHanText(decodeCtc(logits, timeSteps, classes));
    }
    catch (const std::exception &e)
    {
        qWarning() << "ONNX inference failed:" << e.what();
        return QString();
    }
}

bool OcrEngine::isReady() const
{
    return initialized_;
}
