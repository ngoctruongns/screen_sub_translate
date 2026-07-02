#include "translate_client.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

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

bool containsHanCharacters(const QString &text)
{
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        const bool isHan =
            (u >= 0x3400 && u <= 0x4DBF) ||
            (u >= 0x4E00 && u <= 0x9FFF) ||
            (u >= 0xF900 && u <= 0xFAFF);
        if (isHan) {
            return true;
        }
    }
    return false;
}

QString loadPromptContext(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    return QString::fromUtf8(file.readAll()).trimmed();
}

QString normalizeTranslation(QString text)
{
    text = text.trimmed();

    if (text.startsWith(QStringLiteral("```"))) {
        const QStringList parts = text.split(QStringLiteral("```"), Qt::SkipEmptyParts);
        if (!parts.isEmpty()) {
            text = parts.first().trimmed();
        }
    }

    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    if (!lines.isEmpty()) {
        text = lines.first().trimmed();
    }

    static const QStringList prefixes = {
        QStringLiteral("Vietnamese:"),
        QStringLiteral("Bản dịch:"),
        QStringLiteral("Dịch:"),
        QStringLiteral("Translation:")};
    for (const QString &prefix : prefixes) {
        if (text.startsWith(prefix, Qt::CaseInsensitive)) {
            text = text.mid(prefix.size()).trimmed();
            break;
        }
    }

    if ((text.startsWith('"') && text.endsWith('"')) ||
        (text.startsWith('\'') && text.endsWith('\''))) {
        text = text.mid(1, text.size() - 2).trimmed();
    }

    return text;
}

QString sanitizeFinalTranslation(QString text)
{
    text = normalizeTranslation(text);

    const int asciiColon = text.lastIndexOf(':');
    const int fullWidthColon = text.lastIndexOf(QChar(0xFF1A));
    const int splitAt = std::max(asciiColon, fullWidthColon);
    if (splitAt >= 0 && splitAt + 1 < text.size()) {
        const QString tail = text.mid(splitAt + 1).trimmed();
        if (!tail.isEmpty()) {
            text = tail;
        }
    }

    static const QRegularExpression kHanRegex(
        QStringLiteral("[\\x{3400}-\\x{4DBF}\\x{4E00}-\\x{9FFF}\\x{F900}-\\x{FAFF}]+"));
    text.remove(kHanRegex);

    static const QRegularExpression kMultiSpace(QStringLiteral("\\s{2,}"));
    text.replace(kMultiSpace, QStringLiteral(" "));

    text = text.trimmed();
    while (!text.isEmpty() && (text.front() == ':' || text.front() == '-' || text.front() == '"')) {
        text.remove(0, 1);
        text = text.trimmed();
    }

    return text;
}

QString normalizedBaseUrl(QString url)
{
    while (url.endsWith('/')) {
        url.chop(1);
    }
    return url;
}

QUrl llamaGenerateUrl(const QString &baseUrl)
{
    QString normalized = normalizedBaseUrl(baseUrl);
    if (normalized.endsWith(QStringLiteral("/api/generate"))) {
        return QUrl(normalized);
    }
    if (normalized.endsWith(QStringLiteral("/api"))) {
        normalized += QStringLiteral("/generate");
    } else {
        normalized += QStringLiteral("/api/generate");
    }
    return QUrl(normalized);
}

QString translationPrompt(const QString &sourceText,
                         const QString &contextBlock,
                         const QString &recentDialogueContext)
{
    QString prompt = QStringLiteral(
        "You are translating OCR subtitles for a Chinese historical war film into natural Vietnamese subtitle style.\n"
        "Rules:\n"
        "- Output exactly one Vietnamese subtitle line.\n"
        "- Never copy any Chinese Han characters into the answer.\n"
        "- If the source contains person names, place names, or military titles, render them in Vietnamese-friendly Latin script.\n"
        "- If OCR is noisy, infer the intended Chinese sentence before translating.\n"
        "- Keep the translation concise, natural, and suitable for on-screen subtitles.\n"
        "- No explanations, no notes, no quotes, no extra labels.\n");

    if (!contextBlock.isEmpty()) {
        prompt += QStringLiteral("\nMovie context provided by user:\n") + contextBlock + QStringLiteral("\n");
    }

    if (!recentDialogueContext.isEmpty()) {
        prompt += QStringLiteral("\nRecent subtitle context:\n") + recentDialogueContext + QStringLiteral("\n");
    }

    prompt += QStringLiteral("\nChinese OCR subtitle:\n") + sourceText + QStringLiteral("\n\nVietnamese:");
    return prompt;
}

QString repairPrompt(const QString &sourceText,
                     const QString &draftTranslation,
                     const QString &contextBlock)
{
    QString prompt = QStringLiteral(
        "Rewrite the draft into one natural Vietnamese subtitle line.\n"
        "Rules:\n"
        "- Remove all Chinese Han characters completely.\n"
        "- Use only Vietnamese/Latin script.\n"
        "- Preserve the meaning of the original Chinese source.\n"
        "- No explanations, no notes, no quotes, no extra labels.\n");

    if (!contextBlock.isEmpty()) {
        prompt += QStringLiteral("\nMovie context provided by user:\n") + contextBlock + QStringLiteral("\n");
    }

    prompt += QStringLiteral("\nOriginal Chinese:\n") + sourceText +
              QStringLiteral("\n\nBad draft:\n") + draftTranslation +
              QStringLiteral("\n\nClean Vietnamese:");
    return prompt;
}

QString rescuePrompt(const QString &sourceText,
                    const QString &draftTranslation,
                    const QString &contextBlock,
                    const QString &recentDialogueContext)
{
    QString prompt = QStringLiteral(
        "FINAL RETRY. Output exactly one clean Vietnamese subtitle line.\n"
        "Hard constraints:\n"
        "- Do not include any Chinese characters.\n"
        "- Do not include labels like 'translation', 'dịch', '已被译为'.\n"
        "- Do not include explanations.\n"
        "- Keep it short and natural for subtitle display.\n");

    if (!contextBlock.isEmpty()) {
        prompt += QStringLiteral("\nMovie context provided by user:\n") + contextBlock + QStringLiteral("\n");
    }

    if (!recentDialogueContext.isEmpty()) {
        prompt += QStringLiteral("\nRecent subtitle context:\n") + recentDialogueContext + QStringLiteral("\n");
    }

    if (!draftTranslation.trimmed().isEmpty()) {
        prompt += QStringLiteral("\nBad draft to fix:\n") + draftTranslation + QStringLiteral("\n");
    }

    prompt += QStringLiteral("\nChinese OCR subtitle:\n") + sourceText + QStringLiteral("\n\nVietnamese:");
    return prompt;
}

} // namespace

TranslateClient::TranslateClient(QObject *parent)
    : QObject(parent),
      networkManager_(new QNetworkAccessManager(this))
{
    connect(networkManager_, &QNetworkAccessManager::finished, this, &TranslateClient::onReplyFinished);

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
    llamaBaseUrl_ = QString::fromUtf8(qgetenv("SST_LLAMA_BASE_URL")).trimmed();
    if (llamaBaseUrl_.isEmpty()) {
        llamaBaseUrl_ = QString::fromUtf8(tuning::kLlamaBaseUrl);
    }

    llamaModel_ = QString::fromUtf8(qgetenv("SST_LLAMA_MODEL")).trimmed();
    if (llamaModel_.isEmpty()) {
        llamaModel_ = QString::fromUtf8(tuning::kLlamaModel);
    }

    promptContextFilePath_ = QString::fromUtf8(qgetenv("SST_TRANSLATE_CONTEXT_FILE")).trimmed();
    if (promptContextFilePath_.isEmpty()) {
        promptContextFilePath_ = resolveRuntimePath(QString::fromUtf8(tuning::kLlamaContextFilePath));
    }

    localInitialized_ = !llamaModel_.isEmpty() && llamaGenerateUrl(llamaBaseUrl_).isValid();
    if (localInitialized_) {
        qDebug() << "TranslateClient: local Llama backend ready"
                 << "model=" << llamaModel_ << "url=" << llamaGenerateUrl(llamaBaseUrl_).toString()
                 << "context=" << promptContextFilePath_;
    } else {
        qWarning() << "TranslateClient: local Llama backend config invalid"
                   << "model=" << llamaModel_ << "baseUrl=" << llamaBaseUrl_;
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

    backend_ = backend;
    emit backendChanged(backend_ == Backend::GoogleApi ? QStringLiteral("google")
                                                       : QStringLiteral("local-llama"));
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

    if (activeReply_) {
        if (normalized != inFlightText_) {
            pendingText_ = normalized;
        }
        return;
    }

    if (backend_ == Backend::GoogleApi) {
        startGoogleRequest(normalized);
        return;
    }

    if (!localInitialized_) {
        emit translationError(QStringLiteral("Local Llama backend not initialized"));
        return;
    }

    startLlamaRequest(normalized);
}

QString TranslateClient::recentDialogueContext() const
{
    if (recentTranslationHistory_.isEmpty()) {
        return {};
    }

    QStringList lines;
    lines.reserve(recentTranslationHistory_.size() * 2);
    for (const TranslationContextEntry &entry : recentTranslationHistory_) {
        lines.append(QStringLiteral("Chinese: ") + entry.sourceText);
        lines.append(QStringLiteral("Vietnamese: ") + entry.translatedText);
    }

    return lines.join('\n');
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

void TranslateClient::startLlamaRequest(const QString &sourceText)
{
    const QString contextBlock = loadPromptContext(promptContextFilePath_);
    const QString dialogueContext = recentDialogueContext();
    startLlamaPromptRequest(sourceText,
                            translationPrompt(sourceText, contextBlock, dialogueContext),
                            std::nullopt,
                            false,
                            false);
}

void TranslateClient::startLlamaPromptRequest(const QString &sourceText,
                                              const QString &prompt,
                                              const std::optional<QString> &draftTranslation,
                                              bool isRepairPass,
                                              bool isRescuePass)
{
    inFlightText_ = sourceText;

    const QUrl endpoint = llamaGenerateUrl(llamaBaseUrl_);
    QNetworkRequest request(endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QJsonObject payload;
    payload.insert(QStringLiteral("model"), llamaModel_);
    payload.insert(QStringLiteral("prompt"), prompt);
    payload.insert(QStringLiteral("stream"), false);

    QJsonObject options;
    options.insert(QStringLiteral("temperature"), tuning::kLlamaTemperature);
    options.insert(QStringLiteral("num_predict"), tuning::kLlamaNumPredict);
    payload.insert(QStringLiteral("options"), options);

    QNetworkReply *reply =
        networkManager_->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    reply->setProperty("sourceText", sourceText);
    reply->setProperty("backend", QStringLiteral("local"));
    reply->setProperty("repairPass", isRepairPass);
    reply->setProperty("rescuePass", isRescuePass);
    if (draftTranslation.has_value()) {
        reply->setProperty("draftTranslation", *draftTranslation);
    }
    activeReply_ = reply;
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
    reply->setProperty("backend", QStringLiteral("google"));
    activeReply_ = reply;
}

void TranslateClient::onReplyFinished(QNetworkReply *reply)
{
    const QString replyBackend = reply->property("backend").toString();
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
    } else if (replyBackend == QStringLiteral("google")) {
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
                    translated = sanitizeFinalTranslation(translated);
                    if (translated.isEmpty()) {
                        emit translationError(QStringLiteral("Translation is empty after sanitization"));
                        reply->deleteLater();
                        return;
                    }
                    rememberTranslationContext(sourceText, translated);
                    emit translationReady(translated, sourceText);
                }
                reply->deleteLater();
            }
        }
    } else {
        QJsonParseError parseError;
        const QByteArray responseData = reply->readAll();
        const QJsonDocument document = QJsonDocument::fromJson(responseData, &parseError);

        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            emit translationError(QStringLiteral("Failed to parse local Llama response"));
            reply->deleteLater();
        } else {
            const QJsonObject root = document.object();
            QString rawTranslated = root.value(QStringLiteral("response")).toString().trimmed();

            if (rawTranslated.isEmpty()) {
                const QJsonObject messageObj = root.value(QStringLiteral("message")).toObject();
                rawTranslated = messageObj.value(QStringLiteral("content")).toString().trimmed();
            }

            QString translated = sanitizeFinalTranslation(rawTranslated);

            if (translated.isEmpty()) {
                if (!isRescuePass) {
                    const QString contextBlock = loadPromptContext(promptContextFilePath_);
                    const QString dialogueContext = recentDialogueContext();
                    reply->deleteLater();
                    startLlamaPromptRequest(sourceText,
                                            rescuePrompt(sourceText,
                                                         rawTranslated,
                                                         contextBlock,
                                                         dialogueContext),
                                            rawTranslated,
                                            false,
                                            true);
                    return;
                }
                emit translationError(QStringLiteral("Local Llama translation is empty"));
            } else if (containsHanCharacters(translated) && !isRepairPass) {
                const QString contextBlock = loadPromptContext(promptContextFilePath_);
                reply->deleteLater();
                startLlamaPromptRequest(sourceText,
                                        repairPrompt(sourceText, translated, contextBlock),
                                        translated,
                                        true,
                                        false);
                return;
            } else if (containsHanCharacters(translated)) {
                if (!isRescuePass) {
                    const QString contextBlock = loadPromptContext(promptContextFilePath_);
                    const QString dialogueContext = recentDialogueContext();
                    reply->deleteLater();
                    startLlamaPromptRequest(sourceText,
                                            rescuePrompt(sourceText,
                                                         translated,
                                                         contextBlock,
                                                         dialogueContext),
                                            translated,
                                            false,
                                            true);
                    return;
                }
                emit translationError(QStringLiteral("Local Llama output still contains Han after repair"));
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
