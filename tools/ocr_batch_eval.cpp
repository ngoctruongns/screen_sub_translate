#include <QRegularExpression>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <onnxruntime_cxx_api.h>
#include <sentencepiece_processor.h>

#include "ocr_engine.h"
#include "tuning_params.h"

namespace {

QString resolveRuntimePath(const QString &rawPath)
{
    const QFileInfo direct(rawPath);
    if (direct.isAbsolute()) {
        return direct.absoluteFilePath();
    }

    const QString fromCwd = QFileInfo(QDir::current(), rawPath).absoluteFilePath();
    if (QFileInfo::exists(fromCwd)) {
        return fromCwd;
    }

    const QString fromApp = QFileInfo(QCoreApplication::applicationDirPath(), rawPath).absoluteFilePath();
    if (QFileInfo::exists(fromApp)) {
        return fromApp;
    }

    return fromApp;
}

std::map<std::string, QString> loadExpected(const std::string &filePath)
{
    std::ifstream in(filePath);
    std::map<std::string, QString> expected;
    std::string line;

    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }

        const std::size_t dashPos = line.find("- ");
        const std::size_t colonPos = line.find(':');
        if (dashPos == std::string::npos || colonPos == std::string::npos || colonPos <= dashPos + 2) {
            continue;
        }

        std::string key = line.substr(dashPos + 2, colonPos - (dashPos + 2));
        while (!key.empty() && key.back() == ' ') {
            key.pop_back();
        }

        std::string value = line.substr(colonPos + 1);

        while (!value.empty() && value.front() == ' ') {
            value.erase(value.begin());
        }

        expected[key] = QString::fromUtf8(value.c_str()).trimmed();
    }

    return expected;
}

cv::Mat prepareForOcr(const cv::Mat &bgr)
{
    if (bgr.empty()) {
        return {};
    }

    cv::Mat gray;
    if (bgr.channels() == 1) {
        gray = bgr.clone();
    } else {
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    }

    cv::Mat denoised;
    cv::GaussianBlur(gray, denoised, cv::Size(3, 3), 0.0);

    cv::Mat enlarged;
    cv::resize(denoised, enlarged, cv::Size(), 2.6, 2.6, cv::INTER_CUBIC);

    return enlarged;
}

QString normalize(const QString &text)
{
    QString t = text;
    t.remove(QRegularExpression("\\s+"));
    return t.trimmed();
}

class LocalTranslator
{
public:
    LocalTranslator()
        : env_(ORT_LOGGING_LEVEL_WARNING, "ocr_batch_translate_eval")
    {
        try {
            sessionOptions_.SetIntraOpNumThreads(2);
            sessionOptions_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

            const QString modelDir = resolveRuntimePath(QString::fromUtf8(tuning::kTranslateModelDir));
            const QString encPath = modelDir + QStringLiteral("/encoder_model.onnx");
            const QString decPath = modelDir + QStringLiteral("/decoder_model.onnx");
            const QString srcSpm = modelDir + QStringLiteral("/source.spm");
            const QString tgtSpm = modelDir + QStringLiteral("/target.spm");
            const QString vocabPath = modelDir + QStringLiteral("/vocab.json");

            encoderSession_ = std::make_unique<Ort::Session>(
                env_, encPath.toUtf8().constData(), sessionOptions_);
            decoderSession_ = std::make_unique<Ort::Session>(
                env_, decPath.toUtf8().constData(), sessionOptions_);

            srcSP_ = std::make_unique<sentencepiece::SentencePieceProcessor>();
            tgtSP_ = std::make_unique<sentencepiece::SentencePieceProcessor>();

            if (!srcSP_->Load(srcSpm.toStdString()).ok()) {
                error_ = QStringLiteral("Failed to load source.spm: ") + srcSpm;
                return;
            }
            if (!tgtSP_->Load(tgtSpm.toStdString()).ok()) {
                error_ = QStringLiteral("Failed to load target.spm: ") + tgtSpm;
                return;
            }

            QFile vf(vocabPath);
            if (!vf.open(QIODevice::ReadOnly)) {
                error_ = QStringLiteral("Failed to open vocab.json: ") + vocabPath;
                return;
            }
            const QJsonObject obj = QJsonDocument::fromJson(vf.readAll()).object();
            if (obj.isEmpty()) {
                error_ = QStringLiteral("vocab.json is empty or invalid: ") + vocabPath;
                return;
            }

            for (auto it = obj.begin(); it != obj.end(); ++it) {
                const std::string piece = it.key().toStdString();
                const int64_t id = static_cast<int64_t>(it.value().toInt());
                vocabMap_[piece] = id;
                idToVocab_[id] = piece;
            }

            ready_ = true;
        } catch (const std::exception &e) {
            error_ = QStringLiteral("Translator init exception: ") + QString::fromUtf8(e.what());
        }
    }

    bool isReady() const { return ready_; }
    QString lastError() const { return error_; }

    QString translate(const QString &sourceText)
    {
        if (!ready_) {
            return QString();
        }
        const QString normalizedInput = sourceText.trimmed();
        if (normalizedInput.isEmpty()) {
            return QString();
        }

        try {
            Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

            std::vector<std::string> srcPieces;
            const auto encStatus = srcSP_->Encode(normalizedInput.toUtf8().toStdString(), &srcPieces);
            if (!encStatus.ok()) {
                return QString();
            }

            static constexpr int64_t kUnkId = 1;
            std::vector<int64_t> inputIds;
            inputIds.reserve(srcPieces.size() + 2);
            if (tuning::kTranslateSourceLangId >= 0) {
                inputIds.push_back(tuning::kTranslateSourceLangId);
            }
            for (const auto &piece : srcPieces) {
                auto it = vocabMap_.find(piece);
                inputIds.push_back(it != vocabMap_.end() ? it->second : kUnkId);
            }
            inputIds.push_back(tuning::kTranslateEosId);
            if (static_cast<int>(inputIds.size()) > tuning::kTranslateRuntimeMaxInputTokens) {
                inputIds.resize(tuning::kTranslateRuntimeMaxInputTokens);
                inputIds.back() = tuning::kTranslateEosId;
            }

            const int64_t encLen = static_cast<int64_t>(inputIds.size());
            std::vector<int64_t> attentionMask(encLen, 1LL);
            const std::array<int64_t, 2> seqShape{1LL, encLen};

            auto inputIdsTensor = Ort::Value::CreateTensor<int64_t>(
                memInfo, inputIds.data(), inputIds.size(), seqShape.data(), seqShape.size());
            auto attnTensor = Ort::Value::CreateTensor<int64_t>(
                memInfo, attentionMask.data(), attentionMask.size(), seqShape.data(), seqShape.size());

            static constexpr const char *kEncInNames[] = {"input_ids", "attention_mask"};
            static constexpr const char *kEncOutNames[] = {"last_hidden_state"};

            std::array<Ort::Value, 2> encInputs{std::move(inputIdsTensor), std::move(attnTensor)};
            auto encOutputs = encoderSession_->Run(
                Ort::RunOptions{nullptr},
                kEncInNames, encInputs.data(), encInputs.size(),
                kEncOutNames, 1);

            float *hiddenPtr = encOutputs[0].GetTensorMutableData<float>();
            const auto hidShape = encOutputs[0].GetTensorTypeAndShapeInfo().GetShape();
            const int64_t hiddenSize = hidShape[2];

            static constexpr const char *kDecInNames[] = {
                "input_ids", "encoder_attention_mask", "encoder_hidden_states"};
            static constexpr const char *kDecOutNames[] = {"logits"};

            struct BeamState {
                std::vector<int64_t> tokens;
                double logProbSum = 0.0;
                bool ended = false;
            };

            auto normalizedBeamScore = [](const BeamState &beam) {
                const int64_t generatedLen = std::max<int64_t>(1, static_cast<int64_t>(beam.tokens.size()) - 1);
                return beam.logProbSum / std::pow(static_cast<double>(generatedLen), tuning::kTranslateLengthPenalty);
            };

            auto topKLogProbs = [](const float *logits, int64_t vocabSize, int k) {
                std::vector<std::pair<int64_t, double>> result;
                if (vocabSize <= 0 || k <= 0) {
                    return result;
                }

                float maxLogit = -std::numeric_limits<float>::infinity();
                for (int64_t i = 0; i < vocabSize; ++i) {
                    maxLogit = std::max(maxLogit, logits[i]);
                }

                double sumExp = 0.0;
                for (int64_t i = 0; i < vocabSize; ++i) {
                    sumExp += std::exp(static_cast<double>(logits[i] - maxLogit));
                }
                const double logZ = static_cast<double>(maxLogit) + std::log(sumExp);

                result.reserve(static_cast<size_t>(vocabSize));
                for (int64_t i = 0; i < vocabSize; ++i) {
                    result.emplace_back(i, static_cast<double>(logits[i]) - logZ);
                }

                if (static_cast<int>(result.size()) > k) {
                    std::nth_element(
                        result.begin(),
                        result.begin() + k,
                        result.end(),
                        [](const auto &a, const auto &b) { return a.second > b.second; });
                    result.resize(static_cast<size_t>(k));
                }

                std::sort(
                    result.begin(),
                    result.end(),
                    [](const auto &a, const auto &b) { return a.second > b.second; });

                return result;
            };

            std::vector<BeamState> beams;
            beams.push_back(BeamState{{tuning::kTranslateDecoderStartId}, 0.0, false});

            for (int step = 0; step < tuning::kTranslateRuntimeMaxDecodeSteps; ++step) {
                bool allEnded = true;
                std::vector<BeamState> expanded;
                expanded.reserve(static_cast<size_t>(tuning::kTranslateRuntimeNumBeams * tuning::kTranslateRuntimeNumBeams));

                for (const BeamState &beam : beams) {
                    if (beam.ended) {
                        expanded.push_back(beam);
                        continue;
                    }
                    allEnded = false;

                    const int64_t decLen = static_cast<int64_t>(beam.tokens.size());
                    const std::array<int64_t, 2> decShape{1LL, decLen};

                    std::vector<int64_t> decIds = beam.tokens;
                    auto decIdsTensor = Ort::Value::CreateTensor<int64_t>(
                        memInfo, decIds.data(), decIds.size(), decShape.data(), decShape.size());

                    std::vector<int64_t> encMaskCopy(attentionMask);
                    auto encMaskTensor = Ort::Value::CreateTensor<int64_t>(
                        memInfo, encMaskCopy.data(), encMaskCopy.size(), seqShape.data(), seqShape.size());

                    const std::array<int64_t, 3> hidShape3{1LL, encLen, hiddenSize};
                    auto hidTensor = Ort::Value::CreateTensor<float>(
                        memInfo,
                        hiddenPtr,
                        static_cast<size_t>(encLen * hiddenSize),
                        hidShape3.data(),
                        hidShape3.size());

                    std::array<Ort::Value, 3> decInputs{
                        std::move(decIdsTensor), std::move(encMaskTensor), std::move(hidTensor)};

                    auto decOutputs = decoderSession_->Run(
                        Ort::RunOptions{nullptr},
                        kDecInNames, decInputs.data(), decInputs.size(),
                        kDecOutNames, 1);

                    const auto logShape = decOutputs[0].GetTensorTypeAndShapeInfo().GetShape();
                    const int64_t vocabSize = logShape[2];
                    const float *logits = decOutputs[0].GetTensorData<float>();
                    const float *lastRow = logits + (decLen - 1) * vocabSize;

                    std::vector<std::pair<int64_t, double>> candidates;
                    if (decLen == 1 && tuning::kTranslateTargetLangId >= 0) {
                        const int64_t forced = tuning::kTranslateTargetLangId;
                        if (forced >= 0 && forced < vocabSize) {
                            float maxLogit = -std::numeric_limits<float>::infinity();
                            for (int64_t i = 0; i < vocabSize; ++i) {
                                maxLogit = std::max(maxLogit, lastRow[i]);
                            }
                            double sumExp = 0.0;
                            for (int64_t i = 0; i < vocabSize; ++i) {
                                sumExp += std::exp(static_cast<double>(lastRow[i] - maxLogit));
                            }
                            const double logZ = static_cast<double>(maxLogit) + std::log(sumExp);
                            candidates.emplace_back(forced, static_cast<double>(lastRow[forced]) - logZ);
                        }
                    } else {
                        candidates = topKLogProbs(lastRow, vocabSize, tuning::kTranslateRuntimeNumBeams);
                    }

                    for (const auto &[token, logProb] : candidates) {
                        BeamState next = beam;
                        next.tokens.push_back(token);
                        next.logProbSum += logProb;
                        if (token == tuning::kTranslateEosId) {
                            next.ended = true;
                        }
                        expanded.push_back(std::move(next));
                    }
                }

                if (allEnded || expanded.empty()) {
                    break;
                }

                std::sort(
                    expanded.begin(),
                    expanded.end(),
                    [&](const BeamState &a, const BeamState &b) {
                        return normalizedBeamScore(a) > normalizedBeamScore(b);
                    });

                const size_t keepCount = std::min<size_t>(expanded.size(), static_cast<size_t>(tuning::kTranslateRuntimeNumBeams));
                beams.assign(expanded.begin(), expanded.begin() + keepCount);
            }

            if (beams.empty()) {
                return QString();
            }

            const BeamState *best = &beams.front();
            for (const BeamState &beam : beams) {
                if (beam.ended && !best->ended) {
                    best = &beam;
                    continue;
                }
                if (beam.ended == best->ended && normalizedBeamScore(beam) > normalizedBeamScore(*best)) {
                    best = &beam;
                }
            }

            std::vector<int64_t> outIds;
            outIds.reserve(best->tokens.size());
            for (size_t i = 1; i < best->tokens.size(); ++i) {
                const int64_t token = best->tokens[i];
                if (token == tuning::kTranslateEosId) {
                    break;
                }
                if (tuning::kTranslateTargetLangId >= 0 && token == tuning::kTranslateTargetLangId) {
                    continue;
                }
                outIds.push_back(token);
            }

            if (outIds.empty()) {
                return QString();
            }

            std::vector<std::string> outPieces;
            outPieces.reserve(outIds.size());
            for (const int64_t id : outIds) {
                auto it = idToVocab_.find(id);
                if (it != idToVocab_.end()) {
                    outPieces.push_back(it->second);
                }
            }

            std::string decoded;
            const auto decStatus = tgtSP_->Decode(outPieces, &decoded);
            if (!decStatus.ok()) {
                return QString();
            }

            return QString::fromStdString(decoded).trimmed();

        } catch (...) {
            return QString();
        }
    }

private:
    Ort::Env env_;
    Ort::SessionOptions sessionOptions_;
    std::unique_ptr<Ort::Session> encoderSession_;
    std::unique_ptr<Ort::Session> decoderSession_;
    std::unique_ptr<sentencepiece::SentencePieceProcessor> srcSP_;
    std::unique_ptr<sentencepiece::SentencePieceProcessor> tgtSP_;
    std::unordered_map<std::string, int64_t> vocabMap_;
    std::unordered_map<int64_t, std::string> idToVocab_;
    bool ready_ = false;
    QString error_;
};

} // namespace

int main()
{
    const std::string imageDir = "../test/image";
    const std::string expectedFileZh = imageDir + "/image_sub.txt";
    const std::string expectedFileVi = imageDir + "/image_sub_vi.txt";
    const std::string logsDir = "../logs";
    const std::string outputDir = logsDir + "/debug_preprocessed";
    const std::string translationReport = logsDir + "/translation_eval.txt";

    std::filesystem::create_directories(logsDir);
    std::filesystem::create_directories(outputDir);

    const auto expectedZh = loadExpected(expectedFileZh);
    if (expectedZh.empty()) {
        std::cerr << "Failed to load expected labels from " << expectedFileZh << '\n';
        return 1;
    }

    const auto expectedVi = loadExpected(expectedFileVi);
    const bool hasViLabels = !expectedVi.empty();

    OcrEngine engine;
    if (!engine.isReady()) {
        std::cerr << "OCR engine is not ready (check ONNX model and charset paths)\n";
        return 2;
    }

    LocalTranslator translator;
    if (!translator.isReady()) {
        std::cerr << "Translator is not ready: " << translator.lastError().toStdString() << '\n';
        return 3;
    }

    int total = 0;
    int ocrExact = 0;
    int transFromOcrExact = 0;
    int transFromGtExact = 0;

    std::ofstream report(translationReport, std::ios::trunc);
    report << "# OCR + Translation Evaluation\n";
    report << "# zh labels: " << expectedFileZh << "\n";
    report << "# vi labels: " << expectedFileVi << " (" << (hasViLabels ? "found" : "missing") << ")\n\n";

    std::vector<std::string> ids;
    ids.reserve(expectedZh.size());
    for (const auto &[id, _] : expectedZh) {
        ids.push_back(id);
    }

    for (const std::string &id : ids) {
        const std::string imagePath = imageDir + "/" + id + ".png";
        const cv::Mat img = cv::imread(imagePath, cv::IMREAD_COLOR);

        if (img.empty()) {
            std::cerr << "Failed to read image: " << imagePath << '\n';
            continue;
        }

        const auto it = expectedZh.find(id);
        if (it == expectedZh.end()) {
            std::cerr << "Missing label for " << id << '\n';
            continue;
        }

        const cv::Mat prepared = prepareForOcr(img);

        const std::string preparedPath = outputDir + "/" + id + "_prepared.png";
        if (!prepared.empty()) {
            cv::imwrite(preparedPath, prepared);
        }

        const QString predRaw = engine.performOcr(prepared).text.trimmed();
        const QString pred = normalize(predRaw);
        const QString gtRaw = it->second.trimmed();
        const QString gt = normalize(gtRaw);

        const QString viFromOcrRaw = translator.translate(predRaw);
        const QString viFromGtRaw = translator.translate(gtRaw);
        const QString viFromOcr = normalize(viFromOcrRaw);
        const QString viFromGt = normalize(viFromGtRaw);

        ++total;
        const bool ocrOk = (!pred.isEmpty() && pred == gt);
        if (ocrOk) {
            ++ocrExact;
        }

        bool viFromOcrOk = false;
        bool viFromGtOk = false;
        QString viGt;
        if (hasViLabels) {
            auto vit = expectedVi.find(id);
            if (vit != expectedVi.end()) {
                viGt = normalize(vit->second);
                viFromOcrOk = (!viFromOcr.isEmpty() && viFromOcr == viGt);
                viFromGtOk = (!viFromGt.isEmpty() && viFromGt == viGt);
                if (viFromOcrOk) {
                    ++transFromOcrExact;
                }
                if (viFromGtOk) {
                    ++transFromGtExact;
                }
            }
        }

        std::cout << id
                  << " | zh_expected=" << gt.toStdString()
                  << " | zh_ocr=" << pred.toStdString()
                  << " | ocr_match=" << (ocrOk ? "YES" : "NO")
                  << " | vi_from_ocr=" << viFromOcrRaw.toStdString()
                  << " | vi_from_gt=" << viFromGtRaw.toStdString()
                  << (hasViLabels ? (" | vi_expected=" + viGt.toStdString()) : "")
                  << (hasViLabels ? (" | vi_match_from_ocr=" + std::string(viFromOcrOk ? "YES" : "NO")) : "")
                  << (hasViLabels ? (" | vi_match_from_gt=" + std::string(viFromGtOk ? "YES" : "NO")) : "")
                  << " | prepared=" << preparedPath
                  << '\n';

        report << id
               << "\n  zh_expected: " << gtRaw.toStdString()
               << "\n  zh_ocr: " << predRaw.toStdString()
               << "\n  ocr_match: " << (ocrOk ? "YES" : "NO")
               << "\n  vi_from_ocr: " << viFromOcrRaw.toStdString()
               << "\n  vi_from_gt: " << viFromGtRaw.toStdString();
        if (hasViLabels) {
            report << "\n  vi_expected: " << viGt.toStdString()
                   << "\n  vi_match_from_ocr: " << (viFromOcrOk ? "YES" : "NO")
                   << "\n  vi_match_from_gt: " << (viFromGtOk ? "YES" : "NO");
        }
        report << "\n  prepared: " << preparedPath << "\n\n";
    }

    std::cout << "OCR exact match: " << ocrExact << "/" << total << '\n';
    if (hasViLabels) {
        std::cout << "VI exact match (from OCR): " << transFromOcrExact << "/" << total << '\n';
        std::cout << "VI exact match (from GT): " << transFromGtExact << "/" << total << '\n';
    } else {
        std::cout << "VI labels not found at " << expectedFileVi
                  << ". Created translation report for manual review.\n";
    }

    report << "Summary\n";
    report << "  OCR exact: " << ocrExact << "/" << total << "\n";
    if (hasViLabels) {
        report << "  VI exact (from OCR): " << transFromOcrExact << "/" << total << "\n";
        report << "  VI exact (from GT): " << transFromGtExact << "/" << total << "\n";
    } else {
        report << "  VI labels missing -> manual review mode\n";
    }

    std::cout << "Saved preprocessed images to: " << outputDir << '\n';
    std::cout << "Saved translation report to: " << translationReport << '\n';
    return 0;
}
