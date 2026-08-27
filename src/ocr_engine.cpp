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
#include <QStringList>
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

bool isAsciiLetter(const ushort u)
{
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z');
}

bool isCommonSubtitlePunctuation(const ushort u)
{
    // Keep punctuation often seen in subtitles (ASCII + CJK full-width variants).
    static const QString punctuation = QStringLiteral("!\"'(),-.:;?[]{}...…、。！（），：；？");
    return punctuation.contains(QChar(u));
}

// Punctuation that legitimately appears inside or around an English subtitle line.
// Deliberately narrower than the Chinese set: no full-width CJK forms, which in an
// English recognition are always noise.
bool isEnglishSubtitlePunctuation(const ushort u)
{
    static const QString punctuation = QStringLiteral("!\"'(),-.:;?[]…&$%/");
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

OcrEngine::OcrEngine(SourceLanguage language)
    : language_(language),
      profile_(&tuning::profileFor(language)),
      env_(ORT_LOGGING_LEVEL_WARNING, "screen_sub_translate"),
      memoryInfo_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
{
    loadModel(*profile_);
}

OcrEngine::~OcrEngine() = default;

bool OcrEngine::setLanguage(SourceLanguage language)
{
    if (language == language_ && initialized_)
    {
        return true;
    }

    const tuning::LanguageProfile &profile = tuning::profileFor(language);
    if (!loadModel(profile))
    {
        // Leave the engine un-initialized rather than keeping the previous language's
        // model: recognizing English with the Chinese model (or vice versa) produces
        // confident garbage, which is far worse than an explicit "OCR not ready" error.
        qWarning() << "OcrEngine: failed to switch to" << sourcelang::displayName(language)
                   << "model; OCR is now disabled until a valid model is available.";
        language_ = language;
        profile_ = &profile;
        return false;
    }

    language_ = language;
    profile_ = &profile;
    qDebug() << "OcrEngine: switched to" << sourcelang::displayName(language)
             << "model=" << profile.recOnnxPath << "inputWidth=" << profile.inputWidth;
    return true;
}

bool OcrEngine::loadModel(const tuning::LanguageProfile &profile)
{
    initialized_ = false;
    classCountChecked_ = false;
    session_.reset();
    inputNames_.clear();
    outputNames_.clear();
    charset_.clear();
    // The padded canvas is sized from the profile, so the reused buffers from the
    // previous model no longer match; drop them and let performOcr recreate them.
    resizedBuffer_.release();
    canvasBuffer_.release();
    floatImgBuffer_.release();
    inputTensorValues_.clear();

    try
    {
        // Built fresh per load: appending the CUDA execution provider to a reused
        // SessionOptions would stack a second provider entry on every language switch.
        Ort::SessionOptions sessionOptions;
        sessionOptions.SetIntraOpNumThreads(1);
        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

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
                sessionOptions.AppendExecutionProvider_CUDA(cudaOptions);
            }
            catch (const std::exception &e)
            {
                qWarning() << "CUDA EP unavailable, falling back to CPU:" << e.what();
            }
        }

        const QString modelPath = resolveRuntimePath(profile.recOnnxPath);
        const QString charsetPath = resolveRuntimePath(profile.charsetPath);

        if (!QFileInfo::exists(modelPath))
        {
            qWarning() << "Failed to load Paddle model file:" << modelPath;
            return false;
        }

        if (!loadCharset(charsetPath))
        {
            qWarning() << "Failed to load Paddle charset file:" << charsetPath;
            return false;
        }

        session_ = std::make_unique<Ort::Session>(env_, modelPath.toUtf8().constData(), sessionOptions);

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
        return true;
    }
    catch (const std::exception &e)
    {
        qWarning() << "ONNX Runtime init failed:" << e.what();
        initialized_ = false;
        session_.reset();
        return false;
    }
}

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
        // Only the line terminator is stripped, never surrounding whitespace: a dictionary
        // entry can legitimately BE a space character, and trimming it away would erase the
        // entry and shift every later class index by one.
        QString line = in.readLine();
        while (line.endsWith(QLatin1Char('\r')) || line.endsWith(QLatin1Char('\n')))
        {
            line.chop(1);
        }
        charset_.push_back(toUtf8(line));
    }

    // PaddleOCR builds its class list as ['blank'] + <dictionary lines> + [' '] whenever the
    // model was trained with use_space_char: true — which both the Chinese and the English
    // PP-OCRv4/v5 recognition models are. The space is NOT a line in the dictionary file; it
    // is appended after it. Without this the final class index has no mapping, decodeCtc's
    // bounds check drops it, and every space silently disappears from the output. That went
    // unnoticed for Chinese only because normalizeHanText() strips all whitespace anyway.
    charset_.push_back(" ");

    return charset_.size() > 2;
}

QString OcrEngine::decodeCtc(const float *logits, int timeSteps, int classes, float *outConfidence,
                             QString *outPerCharacter) const
{
    if (outConfidence != nullptr)
    {
        *outConfidence = 0.0f;
    }

    if (logits == nullptr || timeSteps <= 0 || classes <= 0)
    {
        return QString();
    }

    std::vector<std::string> pieces;
    std::vector<double> confs;
    pieces.reserve(static_cast<size_t>(timeSteps));
    confs.reserve(static_cast<size_t>(timeSteps));

    int prev = -1;
    for (int t = 0; t < timeSteps; ++t)
    {
        const float *row = logits + static_cast<size_t>(t) * classes;
        int bestIdx = 0;
        float bestVal = row[0];
        double rowSum = row[0];
        for (int c = 1; c < classes; ++c)
        {
            rowSum += row[c];
            if (row[c] > bestVal)
            {
                bestVal = row[c];
                bestIdx = c;
            }
        }

        if (bestIdx != 0 && bestIdx != prev && bestIdx < static_cast<int>(charset_.size()))
        {
            // Per-character confidence. PP-OCRv4 rec normally outputs post-softmax
            // probabilities (row sums to ~1) -> use the winning probability directly.
            // If the export emits raw logits instead, softmax this step on the fly:
            //   p = exp(bestVal) / sum_c exp(row[c]) = 1 / sum_c exp(row[c] - bestVal).
            double stepConf;
            if (std::abs(rowSum - 1.0) < 0.05)
            {
                stepConf = bestVal;
            }
            else
            {
                double denom = 0.0;
                for (int c = 0; c < classes; ++c)
                {
                    denom += std::exp(static_cast<double>(row[c] - bestVal));
                }
                stepConf = (denom > 0.0) ? (1.0 / denom) : 0.0;
            }
            pieces.push_back(charset_[bestIdx]);
            confs.push_back(stepConf);
        }
        prev = bestIdx;
    }

    // Trim leading/trailing low-confidence characters. Dark margins/background that
    // slip into the crop get decoded as a stray edge character (typically 嶺) with
    // far lower confidence than real glyphs. Only the edges are trimmed, so a real
    // line is left intact; interior characters are never dropped.
    const float edgeMinConfidence = profile_->edgeMinConfidence;
    int lo = 0;
    int hi = static_cast<int>(pieces.size());
    while (lo < hi && confs[lo] < edgeMinConfidence)
    {
        ++lo;
    }
    while (hi > lo && confs[hi - 1] < edgeMinConfidence)
    {
        --hi;
    }

    std::string out;
    double confidenceSum = 0.0;
    for (int i = lo; i < hi; ++i)
    {
        out += pieces[i];
        confidenceSum += confs[i];
    }

    // Every decoded character with its score, including the ones the edge gate removed
    // (marked with a leading '~'). Reading this is how edgeMinConfidence gets chosen from
    // data instead of guessed: a hallucinated edge character scores visibly below the rest.
    if (outPerCharacter != nullptr)
    {
        QStringList parts;
        parts.reserve(static_cast<int>(pieces.size()));
        for (int i = 0; i < static_cast<int>(pieces.size()); ++i)
        {
            QString glyph = QString::fromUtf8(pieces[i].c_str());
            if (glyph == QStringLiteral(" "))
            {
                glyph = QStringLiteral("space");
            }
            parts.append(QStringLiteral("%1[%2]%3")
                             .arg(i >= lo && i < hi ? QString() : QStringLiteral("~"),
                                  glyph,
                                  QString::number(confs[i], 'f', 2)));
        }
        *outPerCharacter = parts.join(QLatin1Char(' '));
    }

    const int emittedChars = hi - lo;
    if (outConfidence != nullptr && emittedChars > 0)
    {
        *outConfidence = static_cast<float>(confidenceSum / emittedChars);
    }

    return QString::fromUtf8(out.c_str());
}

QString OcrEngine::normalizeRecognizedText(const QString &text) const
{
    return language_ == SourceLanguage::English ? normalizeLatinText(text) : normalizeHanText(text);
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

QString OcrEngine::normalizeLatinText(const QString &text)
{
    // Unlike the Han path, whitespace must survive: it is the word boundary, and both the
    // stabilization filter and the translation prompt depend on it. Runs of whitespace are
    // collapsed to a single space and any character outside the English subtitle alphabet
    // is dropped (a Han glyph here means the wrong model is loaded, or pure hallucination).
    QString out;
    out.reserve(text.size());

    bool pendingSpace = false;
    for (const QChar c : text.trimmed())
    {
        if (c.isSpace())
        {
            pendingSpace = !out.isEmpty();
            continue;
        }

        const ushort u = c.unicode();
        if (!isAsciiLetter(u) && !isAsciiDigit(u) && !isEnglishSubtitlePunctuation(u))
        {
            continue;
        }

        if (pendingSpace)
        {
            out.append(QLatin1Char(' '));
            pendingSpace = false;
        }
        out.append(c);
    }

    return out.trimmed();
}

OcrEngine::OcrResult OcrEngine::performOcr(const cv::Mat &inputImg)
{
    if (!initialized_ || inputImg.empty() || !session_)
    {
        return {};
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
        return {};
    }

    // Resize the crop to the model height, preserving its aspect ratio.
    const int targetH = tuning::kPaddleInputHeight;
    const int maxW = profile_->inputWidth;

    const cv::Mat subtitleRegion = cropLikelySubtitleRegion(bgr);

    const float regionRatio = static_cast<float>(subtitleRegion.cols) / std::max(1, subtitleRegion.rows);
    int resizedW = static_cast<int>(std::ceil(targetH * regionRatio));
    resizedW = std::clamp(resizedW, 8, maxW);

    // Width of the tensor actually fed to the model. Filling a fixed maxW canvas leaves the
    // tail beyond the text as black padding, and the CTC decoder still walks those timesteps
    // — it can and does emit characters into that dead zone (observed: a trailing '.' at
    // confidence 0.41 on a line whose real characters all scored 1.00). Sizing the canvas to
    // the text removes the dead zone at the source, and costs proportionally less inference.
    int canvasW = maxW;
    if (profile_->adaptiveInputWidth) {
        // Quantised rather than exact: ONNX Runtime re-plans whenever the input shape it has
        // not seen before arrives, and cv::Mat reallocates on every size change. Rounding up
        // keeps the number of distinct shapes small while capping the dead zone at one step.
        constexpr int kWidthQuantum = 64;
        canvasW = ((resizedW + kWidthQuantum - 1) / kWidthQuantum) * kWidthQuantum;
        canvasW = std::clamp(canvasW, kWidthQuantum, maxW);
    }

    resizedBuffer_.create(targetH, resizedW, CV_8UC3);
    cv::resize(subtitleRegion, resizedBuffer_, cv::Size(resizedW, targetH), 0, 0, cv::INTER_LINEAR);

    canvasBuffer_.create(targetH, canvasW, CV_8UC3);
    canvasBuffer_.setTo(cv::Scalar(0, 0, 0));
    resizedBuffer_.copyTo(canvasBuffer_(cv::Rect(0, 0, std::min(resizedW, canvasW), targetH)));

    // Normalize mean=0.5 std=0.5 and pack as planar NCHW float.
    floatImgBuffer_.create(targetH, canvasW, CV_32FC3);
    canvasBuffer_.convertTo(floatImgBuffer_, CV_32FC3, 1.0 / 255.0);

    constexpr int C = 3;
    const size_t planeSize = static_cast<size_t>(targetH) * canvasW;
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

    const std::array<int64_t, 4> shape = {1, C, targetH, canvasW};
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
            return {};
        }

        const Ort::TensorTypeAndShapeInfo info = outputTensors[0].GetTensorTypeAndShapeInfo();
        const std::vector<int64_t> dims = info.GetShape();
        if (dims.size() < 3)
        {
            return {};
        }

        const int timeSteps = static_cast<int>(dims[dims.size() - 2]);
        const int classes = static_cast<int>(dims[dims.size() - 1]);
        const float *logits = outputTensors[0].GetTensorData<float>();

        // One-time sanity check. Decoding maps a class index straight to charset_[index], so
        // a charset that does not match the model produces silent corruption rather than an
        // error: indices past the end are dropped, and a missing entry shifts every later
        // character. Report the mismatch once instead of leaving it to be inferred from
        // garbled output.
        if (!classCountChecked_)
        {
            classCountChecked_ = true;
            if (classes != static_cast<int>(charset_.size()))
            {
                qWarning() << "OcrEngine: charset/model mismatch —" << profile_->charsetPath
                           << "gives" << charset_.size() << "classes but the model outputs"
                           << classes
                           << ". Characters will be dropped or shifted. Check that the .onnx"
                           << "and the dictionary are the matched pair for this language.";
            }
        }

        float confidence = 0.0f;
        QString perCharacter;
        const QString decoded = decodeCtc(logits, timeSteps, classes, &confidence,
                                          perCharacterDebug_ ? &perCharacter : nullptr);
        return {normalizeRecognizedText(decoded), confidence, perCharacter};
    }
    catch (const std::exception &e)
    {
        qWarning() << "ONNX inference failed:" << e.what();
        return {};
    }
}

bool OcrEngine::isReady() const
{
    return initialized_;
}
