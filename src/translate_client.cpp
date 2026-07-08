#include "translate_client.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include "translation_backend_adapter.h"
#include "translation_text_processor.h"
#include "tuning_params.h"

namespace
{

QString resolveRuntimePath(const QString &relative)
{
    const QFileInfo direct(relative);
    if (direct.isAbsolute()) {
        return direct.absoluteFilePath();
    }

    const QString fromCwd = QFileInfo(QDir::current(), relative).absoluteFilePath();
    if (QFileInfo::exists(fromCwd)) {
        return fromCwd;
    }

    return QFileInfo(QCoreApplication::applicationDirPath(), relative).absoluteFilePath();
}

} // namespace

TranslateClient::TranslateClient(QObject *parent)
    : QObject(parent),
      networkManager_(new QNetworkAccessManager(this))
{
    connect(networkManager_, &QNetworkAccessManager::finished, this, &TranslateClient::onReplyFinished);

    initializeTranslationBackend();
}

TranslateClient::~TranslateClient() = default;

void TranslateClient::initializeTranslationBackend()
{
    backendConfigPath_ = resolveRuntimePath(QString::fromUtf8(tuning::kTranslateBackendConfigPath));
    const TranslationBackend::BackendConfig runtimeConfig =
        TranslationBackend::loadBackendConfig(backendConfigPath_);

    backendBaseUrl_ = runtimeConfig.baseUrl;
    backendModel_ = runtimeConfig.model;
    backendApiMode_ = runtimeConfig.apiMode;
    promptContextFilePath_ = runtimeConfig.contextFilePath;
    glossaryFilePath_ = runtimeConfig.glossaryFilePath;
    autoDiscoverModel_ = runtimeConfig.autoDiscoverModel;
    cachePrompt_ = runtimeConfig.cachePrompt;
    repeatLastN_ = runtimeConfig.repeatLastN;
    modelDiscoveryTimeoutMs_ = runtimeConfig.modelDiscoveryTimeoutMs;
    repeatPenalty_ = runtimeConfig.repeatPenalty;
    frequencyPenalty_ = runtimeConfig.frequencyPenalty;

    const TranslationBackend::ApiMode mode =
        TranslationBackend::resolveApiMode(backendApiMode_, backendBaseUrl_);
    if (backendModel_.isEmpty() && autoDiscoverModel_) {
        const std::optional<QString> discovered =
            TranslationBackend::discoverModel(backendBaseUrl_, mode, modelDiscoveryTimeoutMs_);
        if (discovered.has_value()) {
            backendModel_ = discovered.value();
        }
    }

    localInitialized_ = TranslationBackend::endpointUrl(backendBaseUrl_, mode).isValid();
    glossaryAliasPairs_.clear();
    if (localInitialized_) {
        cachedContextBlock_ = TranslationTextProcessor::loadPromptContext(promptContextFilePath_);
        const TranslationTextProcessor::GlossaryData glossaryData =
            TranslationTextProcessor::loadGlossary(glossaryFilePath_);
        glossaryAliasPairs_ = glossaryData.aliasPairs;
        const QString glossaryBlock = glossaryData.promptLines;
        if (!glossaryBlock.isEmpty()) {
            if (!cachedContextBlock_.isEmpty()) {
                cachedContextBlock_ += QStringLiteral("\n\nTerm glossary (apply when relevant):\n") + glossaryBlock;
            } else {
                cachedContextBlock_ = QStringLiteral("Term glossary (apply when relevant):\n") + glossaryBlock;
            }
        }
        qDebug() << "TranslateClient: local translation backend ready"
                 << "model=" << backendModel_ << "mode=" << TranslationBackend::apiModeName(mode)
                 << "url=" << TranslationBackend::endpointUrl(backendBaseUrl_, mode).toString()
                 << "config=" << backendConfigPath_
                 << "context=" << promptContextFilePath_
                 << "glossary=" << glossaryFilePath_
                 << "aliasRules=" << glossaryAliasPairs_.size();
    } else {
        qWarning() << "TranslateClient: translation backend config invalid"
                   << "baseUrl=" << backendBaseUrl_ << "mode=" << backendApiMode_
                   << "config=" << backendConfigPath_;
    }
}

QString TranslateClient::applyGlossaryAliasNormalization(const QString &translatedText) const
{
    return TranslationTextProcessor::applyGlossaryNormalization(translatedText, glossaryAliasPairs_);
}

void TranslateClient::requestTranslation(const QString &sourceText)
{
    const QString normalized = sourceText.trimmed();
    if (normalized.isEmpty()) {
        return;
    }

    if (!TranslationTextProcessor::isLikelyChineseSubtitle(normalized)) {
        emit translationError(QStringLiteral("Skipped non-Chinese OCR candidate"));
        return;
    }

    if (activeReply_) {
        if (normalized != inFlightText_) {
            pendingText_ = normalized;
        }
        return;
    }

    if (!localInitialized_) {
        emit translationError(QStringLiteral("Translation backend not initialized"));
        return;
    }

    startBackendRequest(normalized);
}

QString TranslateClient::recentDialogueContext() const
{
    if (recentTranslationHistory_.isEmpty()) {
        return {};
    }

    const int historySize = static_cast<int>(recentTranslationHistory_.size());
    QStringList lines;
    const int startIndex = std::max(0, historySize - tuning::kTranslateHistoryWindowSize);
    lines.reserve(historySize - startIndex);
    for (int i = startIndex; i < historySize; ++i) {
        const TranslationContextEntry &entry = recentTranslationHistory_.at(i);
        // Only include the Chinese source lines, NOT the Vietnamese translations.
        // Feeding translated text back risks propagating any dirty/garbage output
        // from a previous pass as a "correct" example, creating a feedback loop.
        lines.append(TranslationTextProcessor::shortText(entry.sourceText, tuning::kTranslateHistoryEntryMaxCharsHan));
    }

    return lines.join(QStringLiteral(", "));
}

void TranslateClient::rememberTranslationContext(const QString &sourceText,
                                                 const QString &translatedText)
{
    if (sourceText.trimmed().isEmpty() || translatedText.trimmed().isEmpty()) {
        return;
    }

    if (!recentTranslationHistory_.isEmpty()) {
        const TranslationContextEntry &last = recentTranslationHistory_.constLast();
        if (last.sourceText == sourceText && last.translatedText == translatedText) {
            return;
        }
    }

    recentTranslationHistory_.push_back({sourceText, translatedText});
    while (recentTranslationHistory_.size() > tuning::kRecentSubtitleWindowSize) {
        recentTranslationHistory_.removeFirst();
    }
}

void TranslateClient::startBackendRequest(const QString &sourceText)
{
    const QString dialogueContext = recentDialogueContext();
    startBackendPromptRequest(sourceText,
                              TranslationTextProcessor::translationPrompt(sourceText, cachedContextBlock_, dialogueContext),
                              std::nullopt,
                              false,
                              false);
}

void TranslateClient::startBackendPromptRequest(const QString &sourceText,
                                                const QString &prompt,
                                                const std::optional<QString> &draftTranslation,
                                                bool isRepairPass,
                                                bool isRescuePass)
{
    inFlightText_ = sourceText;

    const TranslationBackend::ApiMode mode = TranslationBackend::resolveApiMode(backendApiMode_, backendBaseUrl_);
    if (backendModel_.isEmpty() && autoDiscoverModel_) {
        const std::optional<QString> discovered = TranslationBackend::discoverModel(backendBaseUrl_, mode, modelDiscoveryTimeoutMs_);
        if (discovered.has_value()) {
            backendModel_ = discovered.value();
            qDebug() << "TranslateClient: discovered model=" << backendModel_
                     << "mode=" << TranslationBackend::apiModeName(mode);
        }
    }

    if (backendModel_.isEmpty() && (mode == TranslationBackend::ApiMode::Ollama || mode == TranslationBackend::ApiMode::OpenAI)) {
        emit translationError(QStringLiteral("No translation model configured/discovered for selected API mode"));
        inFlightText_.clear();
        return;
    }

    const QUrl endpoint = TranslationBackend::endpointUrl(backendBaseUrl_, mode);
    QNetworkRequest request(endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QJsonObject payload;
    if (mode == TranslationBackend::ApiMode::Ollama) {
        if (!backendModel_.isEmpty()) {
            payload.insert(QStringLiteral("model"), backendModel_);
        }
        payload.insert(QStringLiteral("prompt"), prompt);
        payload.insert(QStringLiteral("stream"), false);

        QJsonObject options;
        options.insert(QStringLiteral("temperature"), tuning::kTranslateTemperature);
        options.insert(QStringLiteral("num_predict"), tuning::kTranslateNumPredict);
        options.insert(QStringLiteral("stop"), QJsonArray{QStringLiteral("\n\n"), QStringLiteral("###")});
        payload.insert(QStringLiteral("options"), options);
    } else if (mode == TranslationBackend::ApiMode::OpenAI) {
        if (!backendModel_.isEmpty()) {
            payload.insert(QStringLiteral("model"), backendModel_);
        }
        payload.insert(QStringLiteral("temperature"), tuning::kTranslateTemperature);
        payload.insert(QStringLiteral("max_tokens"), tuning::kTranslateNumPredict);

        QJsonArray messages;
        QJsonObject userMsg;
        userMsg.insert(QStringLiteral("role"), QStringLiteral("user"));
        userMsg.insert(QStringLiteral("content"), prompt);
        messages.append(userMsg);
        payload.insert(QStringLiteral("messages"), messages);
        payload.insert(QStringLiteral("stream"), false);
        payload.insert(QStringLiteral("stop"), QJsonArray{QStringLiteral("\n\n"), QStringLiteral("###")});
    } else {
        payload.insert(QStringLiteral("prompt"), prompt);
        payload.insert(QStringLiteral("temperature"), tuning::kTranslateTemperature);
        payload.insert(QStringLiteral("n_predict"), tuning::kTranslateNumPredict); // max token output
        payload.insert(QStringLiteral("stream"), false);    // Disable streaming for simplicity
        payload.insert(QStringLiteral("cache_prompt"), cachePrompt_); // Allow model to cache prompt for faster subsequent calls

        payload.insert(QStringLiteral("repeat_penalty"), repeatPenalty_);
        payload.insert(QStringLiteral("frequency_penalty"), frequencyPenalty_);
        payload.insert(QStringLiteral("repeat_last_n"), repeatLastN_);

        payload.insert(QStringLiteral("stop"),
                       QJsonArray{
                           // Avoid single-newline stop because many models emit '\n' first.
                           // That can terminate generation before any real text is produced.
                           QStringLiteral("\n\n"),
                           QStringLiteral("<|im_end|>"),    // Token end of chat for Qwen
                           QStringLiteral("<|endoftext|>"), // Token end of text for Qwen
                           QStringLiteral("###")            // End of chat for Ollama
                       });
    }

    QNetworkReply *reply =
        networkManager_->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    reply->setProperty("sourceText", sourceText);
    reply->setProperty("localApiMode", TranslationBackend::apiModeName(mode));
    reply->setProperty("repairPass", isRepairPass);
    reply->setProperty("rescuePass", isRescuePass);
    if (draftTranslation.has_value()) {
        reply->setProperty("draftTranslation", *draftTranslation);
    }
    activeReply_ = reply;
}

void TranslateClient::onReplyFinished(QNetworkReply *reply)
{
    const TranslationBackend::ApiMode localMode = TranslationBackend::parseApiMode(reply->property("localApiMode").toString());
    const QString sourceText = reply->property("sourceText").toString();
    const bool isRepairPass = reply->property("repairPass").toBool();
    const bool isRescuePass = reply->property("rescuePass").toBool();

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

        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            emit translationError(QStringLiteral("Failed to parse translation backend response"));
            reply->deleteLater();
        } else {
            const QJsonObject root = document.object();
            QString rawTranslated = TranslationBackend::extractResponseText(root, localMode);

            QString translated = TranslationTextProcessor::sanitizeFinalTranslation(rawTranslated);
            translated = TranslationTextProcessor::postProcessTranslation(translated);
            translated = applyGlossaryAliasNormalization(translated);

            if (TranslationTextProcessor::isSuspiciouslyShortTranslation(sourceText, translated) && !isRescuePass) {
                const QString dialogueContext = recentDialogueContext();
                reply->deleteLater();
                startBackendPromptRequest(sourceText,
                                          TranslationTextProcessor::completeLinePrompt(sourceText,
                                                             translated,
                                                             cachedContextBlock_,
                                                             dialogueContext),
                                          translated,
                                          false,
                                          true);
                return;
            }

            if (translated.isEmpty()) {
                qWarning() << "TranslateClient: empty local translation after sanitize"
                           << "source=" << TranslationTextProcessor::shortText(sourceText, 40)
                           << "raw=" << TranslationTextProcessor::shortText(rawTranslated, 80)
                           << "mode=" << TranslationBackend::apiModeName(localMode)
                           << "repairPass=" << isRepairPass
                           << "rescuePass=" << isRescuePass;
                if (tuning::kEnableRetryPasses && !isRescuePass) {
                    const QString dialogueContext = recentDialogueContext();
                    reply->deleteLater();
                    startBackendPromptRequest(sourceText,
                                              TranslationTextProcessor::rescuePrompt(sourceText,
                                                           rawTranslated,
                                                           cachedContextBlock_,
                                                           dialogueContext),
                                              rawTranslated,
                                              false,
                                              true);
                    return;
                }
                emit translationError(QStringLiteral("Translation backend output is empty"));
            } else if (TranslationTextProcessor::containsHanCharacters(translated) && !isRepairPass) {
                if (tuning::kEnableRetryPasses) {
                    reply->deleteLater();
                    startBackendPromptRequest(sourceText,
                                              TranslationTextProcessor::repairPrompt(sourceText, translated, cachedContextBlock_),
                                              translated,
                                              true,
                                              false);
                    return;
                }
                emit translationError(QStringLiteral("Translation backend output contains Han"));
            } else if (TranslationTextProcessor::containsHanCharacters(translated)) {
                if (tuning::kEnableRetryPasses && !isRescuePass) {
                    const QString dialogueContext = recentDialogueContext();
                    reply->deleteLater();
                    startBackendPromptRequest(sourceText,
                                              TranslationTextProcessor::rescuePrompt(sourceText,
                                                           translated,
                                                           cachedContextBlock_,
                                                           dialogueContext),
                                              translated,
                                              false,
                                              true);
                    return;
                }
                emit translationError(QStringLiteral("Translation backend output still contains Han after repair"));
            } else if (TranslationTextProcessor::isEnglishHeavyOutput(translated)) {
                emit translationError(QStringLiteral("Translation backend output rejected: English-heavy output"));
            } else if (TranslationTextProcessor::isOverExpandedTranslation(sourceText, translated)) {
                emit translationError(QStringLiteral("Translation backend output rejected: over-expanded short source translation"));
            } else if (TranslationTextProcessor::isSuspiciouslyShortTranslation(sourceText, translated)) {
                emit translationError(QStringLiteral("Translation backend output rejected: suspiciously short translation"));
            } else {
                rememberTranslationContext(sourceText, translated);
                emit translationReady(translated, sourceText);
            }

            reply->deleteLater();
        }
    }

    if (!pendingText_.isEmpty()) {
        const QString next = pendingText_;
        pendingText_.clear();
        requestTranslation(next);
    }
}
