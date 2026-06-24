#include "translate_client.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QtConcurrent/QtConcurrentRun>

#include "tuning_params.h"

namespace
{

QString resolveModelPath(const QString &relative)
{
    const QFileInfo direct(relative);
    if (direct.isAbsolute() && direct.exists()) {
        return direct.absoluteFilePath();
    }

    const QString fromCwd = QFileInfo(QDir::current(), relative).absoluteFilePath();
    if (QFileInfo::exists(fromCwd)) {
        return fromCwd;
    }

    const QString fromApp =
        QFileInfo(QCoreApplication::applicationDirPath(), relative).absoluteFilePath();
    return fromApp;
}

static constexpr char kErrorPrefix[] = "__ERR__:";

bool isLikelyChineseSubtitle(const QString &text)
{
    int cjkCount = 0;
    int letterOrDigitCount = 0;
    for (const QChar ch : text) {
        if (ch.isSpace()) {
            continue;
        }

        const ushort u = ch.unicode();
        const bool isCjk =
            (u >= 0x3400 && u <= 0x4DBF) ||
            (u >= 0x4E00 && u <= 0x9FFF) ||
            (u >= 0xF900 && u <= 0xFAFF);
        if (isCjk) {
            ++cjkCount;
        }
        if (ch.isLetterOrNumber()) {
            ++letterOrDigitCount;
        }
    }

    return cjkCount >= 2 && cjkCount * 2 >= std::max(1, letterOrDigitCount);
}

struct BeamState
{
    std::vector<int64_t> tokens;
    double logProbSum = 0.0;
    bool ended = false;
};

double normalizedBeamScore(const BeamState &beam)
{
    const int64_t generatedLen = std::max<int64_t>(1, static_cast<int64_t>(beam.tokens.size()) - 1);
    return beam.logProbSum / std::pow(static_cast<double>(generatedLen), tuning::kTranslateLengthPenalty);
}

std::vector<std::pair<int64_t, double>> topKLogProbs(const float *logits, int64_t vocabSize, int k)
{
    if (vocabSize <= 0 || k <= 0) {
        return {};
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

    std::vector<std::pair<int64_t, double>> scored;
    scored.reserve(static_cast<size_t>(vocabSize));
    for (int64_t i = 0; i < vocabSize; ++i) {
        scored.emplace_back(i, static_cast<double>(logits[i]) - logZ);
    }

    if (static_cast<int>(scored.size()) > k) {
        std::nth_element(
            scored.begin(),
            scored.begin() + k,
            scored.end(),
            [](const auto &a, const auto &b) { return a.second > b.second; });
        scored.resize(static_cast<size_t>(k));
    }

    std::sort(
        scored.begin(),
        scored.end(),
        [](const auto &a, const auto &b) { return a.second > b.second; });

    return scored;
}

QString runInference(Ort::Session *encoderSession,
                     Ort::Session *decoderSession,
                     sentencepiece::SentencePieceProcessor *srcSP,
                     sentencepiece::SentencePieceProcessor *tgtSP,
                     const std::unordered_map<std::string, int64_t> *vocabMap,
                     const std::unordered_map<int64_t, std::string> *idToVocab,
                     const QString &sourceText)
{
    try {
        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        std::vector<std::string> pieceStrings;
        const auto spStatus = srcSP->Encode(sourceText.toUtf8().toStdString(), &pieceStrings);
        if (!spStatus.ok()) {
            return QLatin1String(kErrorPrefix) + QString::fromStdString(spStatus.ToString());
        }

        static constexpr int64_t kUnkId = 1;
        const int64_t kEosId = tuning::kTranslateEosId;

        std::vector<int64_t> inputIds;
        inputIds.reserve(pieceStrings.size() + 2);
        if (tuning::kTranslateSourceLangId >= 0) {
            inputIds.push_back(tuning::kTranslateSourceLangId);
        }
        for (const auto &piece : pieceStrings) {
            auto it = vocabMap->find(piece);
            inputIds.push_back(it != vocabMap->end() ? it->second : kUnkId);
        }
        inputIds.push_back(kEosId);

        if (static_cast<int>(inputIds.size()) > tuning::kTranslateRuntimeMaxInputTokens) {
            inputIds.resize(tuning::kTranslateRuntimeMaxInputTokens);
            inputIds.back() = kEosId;
        }

        const int64_t encLen = static_cast<int64_t>(inputIds.size());
        std::vector<int64_t> attentionMask(encLen, 1LL);

        const std::array<int64_t, 2> seqShape{1LL, encLen};

        auto inputIdsTensor = Ort::Value::CreateTensor<int64_t>(
            memInfo, inputIds.data(), inputIds.size(), seqShape.data(), seqShape.size());
        auto attentionMaskTensor = Ort::Value::CreateTensor<int64_t>(
            memInfo, attentionMask.data(), attentionMask.size(), seqShape.data(), seqShape.size());

        static constexpr const char *kEncInNames[] = {"input_ids", "attention_mask"};
        static constexpr const char *kEncOutNames[] = {"last_hidden_state"};

        std::array<Ort::Value, 2> encInputs{std::move(inputIdsTensor),
                                            std::move(attentionMaskTensor)};
        auto encOutputs = encoderSession->Run(Ort::RunOptions{nullptr},
                                              kEncInNames, encInputs.data(), encInputs.size(),
                                              kEncOutNames, 1);

        float *hiddenPtr = encOutputs[0].GetTensorMutableData<float>();
        const auto hidShape = encOutputs[0].GetTensorTypeAndShapeInfo().GetShape();
        const int64_t hiddenSize = hidShape[2];

        static constexpr const char *kDecInNames[] = {
            "input_ids", "encoder_attention_mask", "encoder_hidden_states"};
        static constexpr const char *kDecOutNames[] = {"logits"};

        std::vector<BeamState> beams;
        beams.push_back(BeamState{{tuning::kTranslateDecoderStartId}, 0.0, false});

        for (int step = 0; step < tuning::kTranslateRuntimeMaxDecodeSteps; ++step) {
            bool allEnded = true;
            std::vector<BeamState> expanded;
            expanded.reserve(static_cast<size_t>(
                tuning::kTranslateRuntimeNumBeams * tuning::kTranslateRuntimeNumBeams));

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
                    memInfo, hiddenPtr, static_cast<size_t>(encLen * hiddenSize),
                    hidShape3.data(), hidShape3.size());

                std::array<Ort::Value, 3> decInputs{std::move(decIdsTensor),
                                                    std::move(encMaskTensor),
                                                    std::move(hidTensor)};

                auto decOutputs = decoderSession->Run(Ort::RunOptions{nullptr},
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
                [](const BeamState &a, const BeamState &b) {
                    return normalizedBeamScore(a) > normalizedBeamScore(b);
                });

            const size_t keepCount = std::min<size_t>(
                expanded.size(), static_cast<size_t>(tuning::kTranslateRuntimeNumBeams));
            beams.assign(expanded.begin(), expanded.begin() + keepCount);
        }

        if (beams.empty()) {
            return QLatin1String(kErrorPrefix) + QStringLiteral("Empty beam-search result");
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

        std::vector<int64_t> outputIds;
        outputIds.reserve(best->tokens.size());
        for (size_t i = 1; i < best->tokens.size(); ++i) {
            const int64_t token = best->tokens[i];
            if (token == tuning::kTranslateEosId) {
                break;
            }
            if (tuning::kTranslateTargetLangId >= 0 && token == tuning::kTranslateTargetLangId) {
                continue;
            }
            outputIds.push_back(token);
        }

        if (outputIds.empty()) {
            return QLatin1String(kErrorPrefix) + QStringLiteral("Empty translation output");
        }

        std::vector<std::string> outPieces;
        outPieces.reserve(outputIds.size());
        for (int64_t id : outputIds) {
            auto it = idToVocab->find(id);
            if (it != idToVocab->end()) {
                outPieces.push_back(it->second);
            }
        }

        std::string decoded;
        const auto decStatus = tgtSP->Decode(outPieces, &decoded);
        if (!decStatus.ok()) {
            return QLatin1String(kErrorPrefix) + QString::fromStdString(decStatus.ToString());
        }

        const QString result = QString::fromStdString(decoded).trimmed();
        return result.isEmpty() ? QLatin1String(kErrorPrefix) + QStringLiteral("Translation is empty") : result;

    } catch (const std::exception &e) {
        return QLatin1String(kErrorPrefix) + QString::fromUtf8(e.what());
    }
}

} // namespace

TranslateClient::TranslateClient(QObject *parent)
    : QObject(parent),
      networkManager_(new QNetworkAccessManager(this)),
      env_(ORT_LOGGING_LEVEL_WARNING, "translate_client"),
      watcher_(new QFutureWatcher<QString>(this))
{
    connect(networkManager_, &QNetworkAccessManager::finished, this, &TranslateClient::onReplyFinished);

    connect(watcher_, &QFutureWatcher<QString>::finished, this, [this]() {
        const QString result = watcher_->result();
        const QString source = inFlightText_;
        busy_ = false;
        inFlightText_.clear();

        if (result.startsWith(QLatin1String(kErrorPrefix))) {
            emit translationError(result.mid(static_cast<int>(sizeof(kErrorPrefix) - 1)));
        } else {
            emit translationReady(result, source);
        }

        if (!pendingText_.isEmpty()) {
            const QString next = pendingText_;
            pendingText_.clear();
            requestTranslation(next);
        }
    });

    initializeLocalBackend();

    const QByteArray envBackend = qgetenv("SST_TRANSLATE_BACKEND").trimmed().toLower();
    if (envBackend == "google") {
        setBackend(Backend::GoogleApi);
    } else {
        setBackend(Backend::Local);
    }
}

TranslateClient::~TranslateClient() = default;

void TranslateClient::initializeLocalBackend()
{
    try {
        sessionOptions_.SetIntraOpNumThreads(2);
        sessionOptions_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

        const QString dir = resolveModelPath(QString::fromUtf8(tuning::kTranslateModelDir));
        const QString encPath = dir + QStringLiteral("/encoder_model.onnx");
        const QString decPath = dir + QStringLiteral("/decoder_model.onnx");
        const QString srcSpm = dir + QStringLiteral("/source.spm");
        const QString tgtSpm = dir + QStringLiteral("/target.spm");
        const QString vocabPath = dir + QStringLiteral("/vocab.json");

        encoderSession_ = std::make_unique<Ort::Session>(
            env_, encPath.toUtf8().constData(), sessionOptions_);
        decoderSession_ = std::make_unique<Ort::Session>(
            env_, decPath.toUtf8().constData(), sessionOptions_);

        srcSP_ = std::make_unique<sentencepiece::SentencePieceProcessor>();
        tgtSP_ = std::make_unique<sentencepiece::SentencePieceProcessor>();

        if (!srcSP_->Load(srcSpm.toStdString()).ok()) {
            qWarning() << "TranslateClient: failed to load source.spm from" << srcSpm;
            return;
        }
        if (!tgtSP_->Load(tgtSpm.toStdString()).ok()) {
            qWarning() << "TranslateClient: failed to load target.spm from" << tgtSpm;
            return;
        }

        QFile vocabFile(vocabPath);
        if (!vocabFile.open(QIODevice::ReadOnly)) {
            qWarning() << "TranslateClient: failed to open vocab.json from" << vocabPath;
            return;
        }
        const QJsonObject vocabObj = QJsonDocument::fromJson(vocabFile.readAll()).object();
        if (vocabObj.isEmpty()) {
            qWarning() << "TranslateClient: vocab.json is empty or invalid";
            return;
        }

        vocabMap_.clear();
        idToVocab_.clear();
        for (auto it = vocabObj.begin(); it != vocabObj.end(); ++it) {
            const int64_t id = static_cast<int64_t>(it.value().toInt());
            const std::string piece = it.key().toStdString();
            vocabMap_[piece] = id;
            idToVocab_[id] = piece;
        }

        localInitialized_ = true;
        qDebug() << "TranslateClient: local ONNX model ready";
    } catch (const std::exception &e) {
        qWarning() << "TranslateClient: local init failed:" << e.what();
    }
}

void TranslateClient::setBackend(Backend backend)
{
    if (backend_ == backend) {
        return;
    }

    if (activeReply_) {
        activeReply_->abort();
        activeReply_.clear();
    }

    pendingText_.clear();
    inFlightText_.clear();
    busy_ = false;

    backend_ = backend;
    emit backendChanged(backend_ == Backend::GoogleApi ? QStringLiteral("google")
                                                       : QStringLiteral("local"));
}

TranslateClient::Backend TranslateClient::backend() const
{
    return backend_;
}

void TranslateClient::requestTranslation(const QString &sourceText)
{
    const QString normalized = sourceText.trimmed();
    if (normalized.isEmpty()) {
        return;
    }

    if (!isLikelyChineseSubtitle(normalized)) {
        emit translationError(QStringLiteral("Skipped non-Chinese OCR candidate"));
        return;
    }

    if (backend_ == Backend::GoogleApi) {
        if (activeReply_) {
            if (normalized != inFlightText_) {
                pendingText_ = normalized;
            }
            return;
        }
        startGoogleRequest(normalized);
        return;
    }

    if (!localInitialized_) {
        emit translationError(QStringLiteral("Local translation model not initialized"));
        return;
    }

    if (busy_) {
        if (normalized != inFlightText_) {
            pendingText_ = normalized;
        }
        return;
    }

    startInference(normalized);
}

void TranslateClient::startGoogleRequest(const QString &sourceText)
{
    inFlightText_ = sourceText;

    QUrl url(QStringLiteral("https://translate.googleapis.com/translate_a/single"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client"), QStringLiteral("gtx"));
    query.addQueryItem(QStringLiteral("sl"), QStringLiteral("auto"));
    query.addQueryItem(QStringLiteral("tl"), QStringLiteral("vi"));
    query.addQueryItem(QStringLiteral("dt"), QStringLiteral("t"));
    query.addQueryItem(QStringLiteral("q"), sourceText);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = networkManager_->get(request);
    reply->setProperty("sourceText", sourceText);
    activeReply_ = reply;
}

void TranslateClient::onReplyFinished(QNetworkReply *reply)
{
    if (backend_ != Backend::GoogleApi) {
        reply->deleteLater();
        return;
    }

    const QString sourceText = reply->property("sourceText").toString();

    if (activeReply_ == reply) {
        activeReply_.clear();
    }
    inFlightText_.clear();

    if (reply->error() == QNetworkReply::OperationCanceledError) {
        reply->deleteLater();
    } else if (reply->error() != QNetworkReply::NoError) {
        emit translationError(reply->errorString());
        reply->deleteLater();
    } else {
        QJsonParseError parseError;
        const QByteArray responseData = reply->readAll();
        const QJsonDocument document = QJsonDocument::fromJson(responseData, &parseError);

        if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
            emit translationError(QStringLiteral("Failed to parse translation response"));
            reply->deleteLater();
        } else {
            const QJsonArray root = document.array();
            if (root.isEmpty() || !root.at(0).isArray()) {
                emit translationError(QStringLiteral("Unexpected translation response format"));
                reply->deleteLater();
            } else {
                const QJsonArray segments = root.at(0).toArray();
                QString translated;
                for (const QJsonValue &segment : segments) {
                    if (!segment.isArray()) {
                        continue;
                    }
                    const QJsonArray chunk = segment.toArray();
                    if (!chunk.isEmpty() && chunk.at(0).isString()) {
                        translated += chunk.at(0).toString();
                    }
                }

                translated = translated.trimmed();
                if (translated.isEmpty()) {
                    emit translationError(QStringLiteral("Translation is empty"));
                } else {
                    emit translationReady(translated, sourceText);
                }
                reply->deleteLater();
            }
        }
    }

    if (!pendingText_.isEmpty()) {
        const QString next = pendingText_;
        pendingText_.clear();
        requestTranslation(next);
    }
}

void TranslateClient::startInference(const QString &sourceText)
{
    busy_ = true;
    inFlightText_ = sourceText;

    Ort::Session *enc = encoderSession_.get();
    Ort::Session *dec = decoderSession_.get();
    sentencepiece::SentencePieceProcessor *src = srcSP_.get();
    sentencepiece::SentencePieceProcessor *tgt = tgtSP_.get();
    const std::unordered_map<std::string, int64_t> *vm = &vocabMap_;
    const std::unordered_map<int64_t, std::string> *ivm = &idToVocab_;

    watcher_->setFuture(
        QtConcurrent::run([enc, dec, src, tgt, vm, ivm, sourceText]() -> QString {
            return runInference(enc, dec, src, tgt, vm, ivm, sourceText);
        }));
}
