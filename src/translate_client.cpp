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
    glossaryFilePathZh_ = runtimeConfig.glossaryFilePath;
    glossaryFilePathEn_ = runtimeConfig.glossaryFilePathEn;
    configuredSourceLanguage_ = sourcelang::fromKey(runtimeConfig.sourceLanguage);
    sourceLanguage_ = configuredSourceLanguage_;
    autoDiscoverModel_ = runtimeConfig.autoDiscoverModel;
    modelDiscoveryTimeoutMs_ = runtimeConfig.modelDiscoveryTimeoutMs;
    // Sampling, penalties and the token budget come from config/tuning.json (tuning::), not
    // from the backend config, and are read at request time.

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
    glossaryPairs_.clear();
    aliasPairs_.clear();
    if (localInitialized_) {
        loadGlossaryForCurrentLanguage();

        qDebug() << "TranslateClient: local translation backend ready"
                 << "model=" << backendModel_ << "mode=" << TranslationBackend::apiModeName(mode)
                 << "url=" << TranslationBackend::endpointUrl(backendBaseUrl_, mode).toString()
                 << "config=" << backendConfigPath_
                 << "sourceLanguage=" << sourcelang::displayName(sourceLanguage_);
    } else {
        qWarning() << "TranslateClient: translation backend config invalid"
                   << "baseUrl=" << backendBaseUrl_ << "mode=" << backendApiMode_
                   << "config=" << backendConfigPath_;
    }
}

void TranslateClient::loadGlossaryForCurrentLanguage()
{
    const QString path = (sourceLanguage_ == SourceLanguage::English) ? glossaryFilePathEn_
                                                                     : glossaryFilePathZh_;
    const TranslationTextProcessor::AliasData aliasData =
        TranslationTextProcessor::loadAliasRules(path);
    glossaryPairs_ = aliasData.glossaryPairs;
    aliasPairs_ = aliasData.aliasPairs;

    qDebug() << "TranslateClient: glossary loaded"
             << "language=" << sourcelang::displayName(sourceLanguage_)
             << "file=" << path
             << "glossaryRules=" << glossaryPairs_.size()
             << "aliasRules=" << aliasPairs_.size();
}

void TranslateClient::setSourceLanguage(SourceLanguage language)
{
    if (language == sourceLanguage_) {
        return;
    }

    // Cleared BEFORE the abort below: abort() can deliver finished() synchronously, and
    // onReplyFinished() ends by starting a request for pendingText_ — which would build a
    // fresh prompt in the language we are leaving, and land after the switch.
    pendingText_.clear();
    inFlightText_.clear();

    // A request already on the wire was built from the previous language's prompt. Letting
    // it land would judge its response against the new language's quality gate and emit a
    // translation for a line that is no longer on screen, so drop it.
    if (activeReply_) {
        activeReply_->abort(); // onReplyFinished() sees OperationCanceledError and discards it.
    }

    sourceLanguage_ = language;

    // The cache and the dialogue history are keyed by source lines of the previous
    // language; carrying them across would serve a Chinese translation for an English
    // line and feed the prompt context lines the model has no use for.
    translationCache_.clear();
    translationCacheOrder_.clear();
    recentTranslationHistory_.clear();

    loadGlossaryForCurrentLanguage();
}

QString TranslateClient::applyGlossaryAliasNormalization(const QString &translatedText) const
{
    return TranslationTextProcessor::applyAliasNormalization(translatedText, aliasPairs_);
}

void TranslateClient::requestTranslation(const QString &sourceText)
{
    const QString normalized = sourceText.trimmed();
    if (normalized.isEmpty()) {
        return;
    }

    if (!TranslationTextProcessor::isLikelySourceSubtitle(normalized, sourceLanguage_)) {
        emit translationError(QStringLiteral("Skipped OCR candidate that is not %1")
                                  .arg(sourcelang::displayName(sourceLanguage_)));
        return;
    }

    // Serve repeated subtitle lines from cache without hitting the backend.
    const std::optional<QString> cached = lookupTranslationCache(normalized);
    if (cached.has_value()) {
        rememberTranslationContext(normalized, cached.value());
        emit translationReady(cached.value(), normalized);
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
        // Only include the source lines, NOT the Vietnamese translations. Feeding
        // translated text back risks propagating any dirty/garbage output from a
        // previous pass as a "correct" example, creating a feedback loop.
        // The truncation length is per-language: an English line needs far more
        // characters than a Han line to carry the same amount of context.
        lines.append(TranslationTextProcessor::shortText(
            entry.sourceText, tuning::profileFor(sourceLanguage_).historyEntryMaxChars));
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

std::optional<QString> TranslateClient::lookupTranslationCache(const QString &sourceText)
{
    const QString key = sourceText.trimmed();
    const auto it = translationCache_.constFind(key);
    if (it == translationCache_.constEnd()) {
        return std::nullopt;
    }

    // Mark as most-recently-used.
    translationCacheOrder_.removeOne(key);
    translationCacheOrder_.append(key);
    return it.value();
}

void TranslateClient::insertTranslationCache(const QString &sourceText, const QString &translatedText)
{
    if (tuning::kTranslationCacheSize <= 0) {
        return;
    }

    const QString key = sourceText.trimmed();
    if (key.isEmpty() || translatedText.trimmed().isEmpty()) {
        return;
    }

    if (translationCache_.contains(key)) {
        translationCache_[key] = translatedText;
        translationCacheOrder_.removeOne(key);
        translationCacheOrder_.append(key);
        return;
    }

    while (translationCacheOrder_.size() >= tuning::kTranslationCacheSize &&
           !translationCacheOrder_.isEmpty()) {
        translationCache_.remove(translationCacheOrder_.takeFirst());
    }

    translationCache_.insert(key, translatedText);
    translationCacheOrder_.append(key);
}

void TranslateClient::startBackendRequest(const QString &sourceText)
{
    const QString dialogueContext = recentDialogueContext();
    const QString glossaryBlock =
        TranslationTextProcessor::buildGlossaryBlockForSource(sourceText, glossaryPairs_);
    if (!glossaryBlock.isEmpty()) {
        qDebug() << "TranslateClient: glossary block selected" << "source="
                 << TranslationTextProcessor::shortText(sourceText, 36)
                 << "entries=" << glossaryBlock.split('\n', Qt::SkipEmptyParts).size();
    }
    // Do not include the user-provided movie context in per-line prompts by default.
    // For small local models, broad natural-language context can leak into the subtitle output,
    // especially when the OCR source is short or noisy. Glossary entries and recent source
    // lines remain enabled because they are line-specific and less prone to leakage.
    startBackendPromptRequest(sourceText,
                              TranslationTextProcessor::translationPrompt(sourceText,
                                                                           dialogueContext,
                                                                           glossaryBlock,
                                                                           sourceLanguage_),
                              false);
}

void TranslateClient::startBackendPromptRequest(const QString &sourceText,
                                                const QString &prompt,
                                                bool isRetryPass,
                                                const QString &priorSalvage)
{
    inFlightText_ = sourceText;

    // Model discovery is performed once during initializeTranslationBackend(). Doing it here
    // would run a nested synchronous QEventLoop inside onReplyFinished() (reentrancy) and block
    // the UI thread on every request, so it is intentionally not repeated in this hot path.
    const TranslationBackend::ApiMode mode = TranslationBackend::resolveApiMode(backendApiMode_, backendBaseUrl_);

    if (backendModel_.isEmpty() && (mode == TranslationBackend::ApiMode::Ollama || mode == TranslationBackend::ApiMode::OpenAI)) {
        emit translationError(QStringLiteral("No translation model configured/discovered for selected API mode"));
        inFlightText_.clear();
        return;
    }

    const double temperature = isRetryPass ? tuning::kTranslateRetryTemperature : tuning::kTranslateTemperature;
    const int topK = isRetryPass ? tuning::kTranslateRetryTopK : tuning::kTranslateTopK;
    const double topP = isRetryPass ? tuning::kTranslateRetryTopP : tuning::kTranslateTopP;
    const double minP = isRetryPass ? tuning::kTranslateRetryMinP : tuning::kTranslateMinP;

    const QUrl endpoint = TranslationBackend::endpointUrl(backendBaseUrl_, mode);
    QNetworkRequest request(endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    // Guard against a frozen/unreachable backend wedging the pipeline: a timed-out reply
    // finishes with an error, clears activeReply_, and lets pendingText_ proceed.
    request.setTransferTimeout(tuning::kTranslateRequestTimeoutMs);

    QJsonObject payload;
    if (mode == TranslationBackend::ApiMode::Ollama) {
        if (!backendModel_.isEmpty()) {
            payload.insert(QStringLiteral("model"), backendModel_);
        }
        payload.insert(QStringLiteral("prompt"), prompt);
        payload.insert(QStringLiteral("stream"), false);

        QJsonObject options;
        options.insert(QStringLiteral("temperature"), temperature);
        options.insert(QStringLiteral("num_predict"), tuning::kTranslateNumPredict);
        options.insert(QStringLiteral("top_k"), topK);
        options.insert(QStringLiteral("top_p"), topP);
        options.insert(QStringLiteral("min_p"), minP);
        // No "\n\n" stop here: the chat template ends the turn on its own, and reasoning
        // models (Qwen3) emit "<think></think>\n\n<answer>" — a "\n\n" stop truncates the
        // reply right after the (empty) think block, leaving no translation.
        options.insert(QStringLiteral("stop"), QJsonArray{QStringLiteral("###")});
        payload.insert(QStringLiteral("options"), options);
    } else if (mode == TranslationBackend::ApiMode::OpenAI) {
        if (!backendModel_.isEmpty()) {
            payload.insert(QStringLiteral("model"), backendModel_);
        }
        payload.insert(QStringLiteral("temperature"), temperature);
        payload.insert(QStringLiteral("max_tokens"), tuning::kTranslateNumPredict);
        payload.insert(QStringLiteral("top_p"), topP);

        QJsonArray messages;
        QJsonObject userMsg;
        userMsg.insert(QStringLiteral("role"), QStringLiteral("user"));
        userMsg.insert(QStringLiteral("content"), prompt);
        messages.append(userMsg);
        payload.insert(QStringLiteral("messages"), messages);
        payload.insert(QStringLiteral("stream"), false);
        // No "\n\n" stop: see the Ollama branch — it would cut Qwen3's reply right after
        // the empty "<think></think>" block, before the actual translation.
        payload.insert(QStringLiteral("stop"), QJsonArray{QStringLiteral("###")});
    } else {
        payload.insert(QStringLiteral("prompt"), prompt);
        payload.insert(QStringLiteral("temperature"), temperature);
        payload.insert(QStringLiteral("n_predict"), tuning::kTranslateNumPredict); // max token output
        payload.insert(QStringLiteral("stream"), false);    // Disable streaming for simplicity
        payload.insert(QStringLiteral("cache_prompt"), tuning::kTranslateCachePrompt); // Allow model to cache prompt for faster subsequent calls

        payload.insert(QStringLiteral("repeat_penalty"), tuning::kTranslateRepeatPenalty);
        payload.insert(QStringLiteral("frequency_penalty"), tuning::kTranslateFrequencyPenalty);
        payload.insert(QStringLiteral("repeat_last_n"), tuning::kTranslateRepeatLastN);

        // Sampling parameters — control the token probability distribution.
        // See BackendConfig in translation_backend_adapter.h for tuning notes.
        payload.insert(QStringLiteral("top_k"), topK);
        payload.insert(QStringLiteral("top_p"), topP);
        payload.insert(QStringLiteral("min_p"), minP);

        payload.insert(QStringLiteral("stop"),
                       QJsonArray{
                            // Do not use single newline as a stop sequence.
                            // Qwen often emits '\n' before the actual answer; stopping on it
                            // can produce an empty completion.
                            QStringLiteral("\n\n"),
                            QStringLiteral("<|im_end|>"),    // Chat turn end token (Qwen/ChatML)
                            QStringLiteral("<|endoftext|>"), // Text end token (Qwen/GPT family)
                            QStringLiteral("<|end|>"),
                            QStringLiteral("<|eot_id|>"),
                            QStringLiteral("###"),         // Section separator often used by Ollama
                            QStringLiteral("\u8d8a\u5357\u8bed:"),        // Suppress bilingual meta-label that Qwen emits in Chinese
                            QStringLiteral("越南语："),
                            QStringLiteral("越南语:"),
                            QStringLiteral("越南语字幕："),
                            QStringLiteral("Note:"),         // Suppress explanatory footnotes
                       });
    }

    QNetworkReply *reply =
        networkManager_->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    reply->setProperty("sourceText", sourceText);
    reply->setProperty("localApiMode", TranslationBackend::apiModeName(mode));
    reply->setProperty("retryPass", isRetryPass);
    reply->setProperty("priorSalvage", priorSalvage);
    activeReply_ = reply;
}

void TranslateClient::onReplyFinished(QNetworkReply *reply)
{
    const TranslationBackend::ApiMode localMode = TranslationBackend::parseApiMode(reply->property("localApiMode").toString());
    const QString sourceText = reply->property("sourceText").toString();
    const bool isRetryPass = reply->property("retryPass").toBool();

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

            // Print to debug raw translate
            qDebug() << "Raw translation output:" << rawTranslated;

            // Strip reasoning-model scaffolding (<think>...</think>) before any quality
            // checks so downstream logic only sees the actual translation.
            rawTranslated = TranslationTextProcessor::stripReasoningBlocks(rawTranslated);

            if (rawTranslated.trimmed().isEmpty()) {
                qWarning().noquote() << "TranslateClient: empty raw response body:"
                                     << QString::fromUtf8(responseData);
            }

            QString translated =
                TranslationTextProcessor::selectBestVietnameseLine(rawTranslated, sourceLanguage_);

            TranslationTextProcessor::TranslationIssue issue = TranslationTextProcessor::TranslationIssue::None;
            if (translated.trimmed().isEmpty() && !rawTranslated.trimmed().isEmpty()) {
                issue = TranslationTextProcessor::containsHanCharacters(rawTranslated)
                    ? TranslationTextProcessor::TranslationIssue::ContainsHan
                    : TranslationTextProcessor::TranslationIssue::NoUsableVietnameseCandidate;
            }

            translated = TranslationTextProcessor::sanitizeFinalTranslation(translated);

            if (issue == TranslationTextProcessor::TranslationIssue::None) {
                issue = TranslationTextProcessor::evaluateTranslationQuality(sourceText, translated,
                                                                            sourceLanguage_);
            }

            const QString priorSalvage = reply->property("priorSalvage").toString();

            // Two cheaply-repairable residues, one per source language: a mostly-Vietnamese
            // line with a few stray Han (Chinese source) or a few untranslated English words
            // (English source). Both are fixed by deleting the leftovers rather than by
            // spending another backend round-trip, so on the retry pass they take the
            // success path directly.
            const bool canFallbackByRemovingHan =
                isRetryPass && issue == TranslationTextProcessor::TranslationIssue::ResidualHan;
            const bool canFallbackByRemovingEnglish =
                isRetryPass && issue == TranslationTextProcessor::TranslationIssue::ResidualEnglish;

            if (issue == TranslationTextProcessor::TranslationIssue::None ||
                canFallbackByRemovingHan || canFallbackByRemovingEnglish) {
                QString finalText = TranslationTextProcessor::removeHanCharacters(translated);
                if (canFallbackByRemovingEnglish) {
                    finalText = TranslationTextProcessor::removeForeignLatinWords(finalText);
                }
                finalText = TranslationTextProcessor::postProcessTranslation(finalText);
                finalText = applyGlossaryAliasNormalization(finalText);

                const TranslationTextProcessor::TranslationIssue finalIssue =
                    TranslationTextProcessor::evaluateTranslationQuality(sourceText, finalText,
                                                                        sourceLanguage_);
                if (finalText.trimmed().isEmpty() ||
                    finalIssue == TranslationTextProcessor::TranslationIssue::Empty ||
                    finalIssue == TranslationTextProcessor::TranslationIssue::TooShort) {
                    // A clean-looking line can still collapse after post-processing; try to
                    // salvage rather than drop the subtitle.
                    emitBestEffortOrError(sourceText, rawTranslated, priorSalvage, finalIssue);
                } else {
                    rememberTranslationContext(sourceText, finalText);
                    insertTranslationCache(sourceText, finalText);
                    emit translationReady(finalText, sourceText);
                }
            } else if (tuning::kEnableRetryPasses && !isRetryPass) {
                qWarning() << "TranslateClient: translation quality check failed, retrying\n"
                           << "issue=" << TranslationTextProcessor::translationIssueMessage(issue) << "\n"
                           << "source=" << TranslationTextProcessor::shortText(sourceText, 40) << "\n"
                           << "translated=" << TranslationTextProcessor::shortText(translated, 80);
                const QString dialogueContext = recentDialogueContext();
                const QString glossaryBlock =
                    TranslationTextProcessor::buildGlossaryBlockForSource(sourceText, glossaryPairs_);
                // Carry the best Vietnamese fragment from this pass forward: if the retry
                // comes back worse (or empty), we can still fall back to it.
                const QString firstPassSalvage =
                    TranslationTextProcessor::salvageVietnameseFragment(rawTranslated);
                reply->deleteLater();
                startBackendPromptRequest(sourceText,
                                          TranslationTextProcessor::translationRetryPrompt(sourceText,
                                                                                           dialogueContext,
                                                                                           rawTranslated,
                                                                                           glossaryBlock,
                                                                                           issue,
                                                                                           sourceLanguage_),
                                          true,
                                          firstPassSalvage);
                return;
            } else {
                // Retry already spent (or disabled) and still failing: recover what we can.
                emitBestEffortOrError(sourceText, rawTranslated, priorSalvage, issue);
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

void TranslateClient::emitBestEffortOrError(const QString &sourceText,
                                            const QString &rawTranslated,
                                            const QString &priorSalvage,
                                            TranslationTextProcessor::TranslationIssue issue)
{
    // Prefer a fragment recovered from this pass; fall back to the earlier pass's fragment
    // when this pass produced nothing Vietnamese-looking at all.
    QString salvaged = TranslationTextProcessor::salvageVietnameseFragment(rawTranslated);
    if (salvaged.trimmed().isEmpty()) {
        salvaged = priorSalvage;
    }

    salvaged = TranslationTextProcessor::postProcessTranslation(salvaged);
    salvaged = applyGlossaryAliasNormalization(salvaged);

    if (salvaged.trimmed().isEmpty()) {
        emit translationError(TranslationTextProcessor::translationIssueMessage(issue));
        return;
    }

    qWarning() << "TranslateClient: emitting best-effort salvaged translation\n"
               << "issue=" << TranslationTextProcessor::translationIssueMessage(issue) << "\n"
               << "source=" << TranslationTextProcessor::shortText(sourceText, 40) << "\n"
               << "salvaged=" << TranslationTextProcessor::shortText(salvaged, 80);

    rememberTranslationContext(sourceText, salvaged);
    // Intentionally NOT cached: salvaged output is degraded, so a future clean pass on the
    // same source line should be free to replace it.
    emit translationReady(salvaged, sourceText);
}
