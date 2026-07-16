#include "translation_backend_adapter.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include "tuning_params.h"

namespace TranslationBackend
{

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

QString jsonStringOrDefault(const QJsonObject &obj,
                            const QString &key,
                            const QString &fallback)
{
    const QJsonValue value = obj.value(key);
    if (!value.isString()) {
        return fallback;
    }

    const QString parsed = value.toString().trimmed();
    return parsed.isEmpty() ? fallback : parsed;
}

bool jsonBoolOrDefault(const QJsonObject &obj, const QString &key, bool fallback)
{
    const QJsonValue value = obj.value(key);
    if (!value.isBool()) {
        return fallback;
    }
    return value.toBool();
}

int jsonIntOrDefault(const QJsonObject &obj, const QString &key, int fallback)
{
    const QJsonValue value = obj.value(key);
    if (!value.isDouble()) {
        return fallback;
    }
    return value.toInt(fallback);
}

double jsonDoubleOrDefault(const QJsonObject &obj, const QString &key, double fallback)
{
    const QJsonValue value = obj.value(key);
    if (!value.isDouble()) {
        return fallback;
    }
    return value.toDouble(fallback);
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

QUrl llamaTagsUrl(const QString &baseUrl)
{
    QString normalized = normalizedBaseUrl(baseUrl);
    if (normalized.endsWith(QStringLiteral("/api/tags"))) {
        return QUrl(normalized);
    }
    if (normalized.endsWith(QStringLiteral("/api"))) {
        normalized += QStringLiteral("/tags");
    } else {
        normalized += QStringLiteral("/api/tags");
    }
    return QUrl(normalized);
}

QUrl openAiChatCompletionsUrl(const QString &baseUrl)
{
    QString normalized = normalizedBaseUrl(baseUrl);
    if (normalized.endsWith(QStringLiteral("/v1/chat/completions"))) {
        return QUrl(normalized);
    }
    if (normalized.endsWith(QStringLiteral("/v1"))) {
        normalized += QStringLiteral("/chat/completions");
    } else {
        normalized += QStringLiteral("/v1/chat/completions");
    }
    return QUrl(normalized);
}

QUrl openAiModelsUrl(const QString &baseUrl)
{
    QString normalized = normalizedBaseUrl(baseUrl);
    if (normalized.endsWith(QStringLiteral("/v1/models"))) {
        return QUrl(normalized);
    }
    if (normalized.endsWith(QStringLiteral("/v1"))) {
        normalized += QStringLiteral("/models");
    } else {
        normalized += QStringLiteral("/v1/models");
    }
    return QUrl(normalized);
}

QUrl llamaCppCompletionUrl(const QString &baseUrl)
{
    QString normalized = normalizedBaseUrl(baseUrl);
    if (normalized.endsWith(QStringLiteral("/completion"))) {
        return QUrl(normalized);
    }
    normalized += QStringLiteral("/completion");
    return QUrl(normalized);
}

} // anonymous namespace

BackendConfig defaultBackendConfig()
{
    BackendConfig config;
    config.baseUrl = QString::fromUtf8(tuning::kTranslateBaseUrl).trimmed();
    config.model = QString::fromUtf8(tuning::kTranslateModel).trimmed();
    config.apiMode = QString::fromUtf8(tuning::kTranslateApiMode).trimmed().toLower();
    config.contextFilePath = resolveRuntimePath(QString::fromUtf8(tuning::kTranslateContextFilePath));
    config.glossaryFilePath = resolveRuntimePath(QString::fromUtf8(tuning::kTranslateGlossaryFilePath));
    config.autoDiscoverModel = true;
    config.cachePrompt = true;
    config.modelDiscoveryTimeoutMs = 1200;
    config.temperature = tuning::kTranslateTemperature;
    config.numPredict = tuning::kTranslateNumPredict;
    config.retryTemperature = tuning::kTranslateRetryTemperature;
    config.repeatLastN = 128;
    config.repeatPenalty = 1.15;
    config.frequencyPenalty = 1.15;
    config.topP = 0.85;
    config.minP = 0.06;
    config.topK = 40;
    return config;
}

BackendConfig loadBackendConfig(const QString &configPath)
{
    BackendConfig config = defaultBackendConfig();

    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "TranslationBackend: config not found, using defaults:" << configPath;
        return config;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "TranslationBackend: failed to parse config:" << configPath
                   << err.errorString();
        return config;
    }

    const QJsonObject root = doc.object();
    config.baseUrl = jsonStringOrDefault(root, QStringLiteral("baseUrl"), config.baseUrl);
    config.model = jsonStringOrDefault(root, QStringLiteral("model"), config.model);
    config.apiMode = jsonStringOrDefault(root, QStringLiteral("apiMode"), config.apiMode).toLower();
    config.contextFilePath = resolveRuntimePath(
        jsonStringOrDefault(root, QStringLiteral("contextFile"), config.contextFilePath));
    config.glossaryFilePath = resolveRuntimePath(
        jsonStringOrDefault(root, QStringLiteral("glossaryFile"), config.glossaryFilePath));
    config.autoDiscoverModel = jsonBoolOrDefault(root, QStringLiteral("autoDiscoverModel"), true);
    config.cachePrompt = jsonBoolOrDefault(root, QStringLiteral("cachePrompt"), true);
    config.temperature = std::clamp(
        jsonDoubleOrDefault(root, QStringLiteral("temperature"), config.temperature), 0.0, 2.0);
    config.numPredict = std::max(1,
        jsonIntOrDefault(root, QStringLiteral("numPredict"), config.numPredict));
    config.retryTemperature = std::clamp(
        jsonDoubleOrDefault(root, QStringLiteral("retryTemperature"), config.retryTemperature), 0.0, 2.0);
    config.repeatLastN = std::max(0, jsonIntOrDefault(root, QStringLiteral("repeatLastN"), 128));
    config.modelDiscoveryTimeoutMs = std::max(200,
        jsonIntOrDefault(root, QStringLiteral("modelDiscoveryTimeoutMs"), 1200));
    config.repeatPenalty = std::max(1.0,
        jsonDoubleOrDefault(root, QStringLiteral("repeatPenalty"), 1.15));
    config.frequencyPenalty = std::max(0.0,
        jsonDoubleOrDefault(root, QStringLiteral("frequencyPenalty"), 1.15));
    config.topP = std::clamp(jsonDoubleOrDefault(root, QStringLiteral("topP"), 0.85), 0.1, 1.0);
    config.minP = std::clamp(jsonDoubleOrDefault(root, QStringLiteral("minP"), 0.06), 0.0, 0.5);
    config.topK = std::max(0, jsonIntOrDefault(root, QStringLiteral("topK"), 40));

    return config;
}

ApiMode parseApiMode(const QString &modeText)
{
    const QString mode = modeText.trimmed().toLower();
    if (mode == QStringLiteral("llamacpp") || mode == QStringLiteral("llama.cpp")) {
        return ApiMode::LlamaCpp;
    }
    if (mode == QStringLiteral("openai") || mode == QStringLiteral("chat")) {
        return ApiMode::OpenAI;
    }
    if (mode == QStringLiteral("ollama")) {
        return ApiMode::Ollama;
    }
    return ApiMode::Auto;
}

ApiMode resolveApiMode(const QString &modeText, const QString &baseUrl)
{
    const ApiMode parsed = parseApiMode(modeText);
    if (parsed != ApiMode::Auto) {
        return parsed;
    }

    const QString url = normalizedBaseUrl(baseUrl).toLower();
    if (url.contains(QStringLiteral(":11434")) || url.contains(QStringLiteral("/api"))) {
        return ApiMode::Ollama;
    }
    if (url.contains(QStringLiteral("/v1"))) {
        return ApiMode::OpenAI;
    }

    return ApiMode::LlamaCpp;
}

QString apiModeName(ApiMode mode)
{
    switch (mode) {
    case ApiMode::LlamaCpp:
        return QStringLiteral("llamacpp");
    case ApiMode::OpenAI:
        return QStringLiteral("openai");
    case ApiMode::Ollama:
        return QStringLiteral("ollama");
    case ApiMode::Auto:
    default:
        return QStringLiteral("auto");
    }
}

QUrl endpointUrl(const QString &baseUrl, ApiMode mode)
{
    switch (mode) {
    case ApiMode::Ollama:
        return llamaGenerateUrl(baseUrl);
    case ApiMode::OpenAI:
        return openAiChatCompletionsUrl(baseUrl);
    case ApiMode::LlamaCpp:
    case ApiMode::Auto:
    default:
        return llamaCppCompletionUrl(baseUrl);
    }
}

QUrl modelDiscoveryUrl(const QString &baseUrl, ApiMode mode)
{
    if (mode == ApiMode::Ollama) {
        return llamaTagsUrl(baseUrl);
    } else if (mode == ApiMode::OpenAI) {
        return openAiModelsUrl(baseUrl);
    }
    return QUrl();
}

std::optional<QString> discoverModel(const QString &baseUrl, ApiMode mode, int timeoutMs)
{
    const QUrl discoverUrl = modelDiscoveryUrl(baseUrl, mode);
    if (!discoverUrl.isValid()) {
        return std::nullopt;
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(discoverUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = manager.get(request);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    timer.start(timeoutMs);
    loop.exec();

    if (timer.isActive()) {
        timer.stop();
    } else {
        reply->abort();
    }

    if (reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        return std::nullopt;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &err);
    reply->deleteLater();
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::nullopt;
    }

    const QJsonObject root = doc.object();
    if (mode == ApiMode::Ollama) {
        const QJsonArray models = root.value(QStringLiteral("models")).toArray();
        for (const QJsonValue &item : models) {
            const QJsonObject modelObject = item.toObject();
            const QString name = modelObject.value(QStringLiteral("name")).toString().trimmed();
            if (!name.isEmpty()) {
                return name;
            }
        }
        return std::nullopt;
    }

    const QJsonArray models = root.value(QStringLiteral("data")).toArray();
    for (const QJsonValue &item : models) {
        const QJsonObject modelObject = item.toObject();
        const QString id = modelObject.value(QStringLiteral("id")).toString().trimmed();
        if (!id.isEmpty()) {
            return id;
        }
    }
    return std::nullopt;
}

QString extractResponseText(const QJsonObject &root, ApiMode mode)
{
    if (mode == ApiMode::Ollama) {
        QString text = root.value(QStringLiteral("response")).toString().trimmed();
        if (text.isEmpty()) {
            text = root.value(QStringLiteral("message")).toObject().value(QStringLiteral("content")).toString().trimmed();
        }
        return text;
    }

    if (mode == ApiMode::LlamaCpp) {
        QString text = root.value(QStringLiteral("content")).toString().trimmed();
        if (!text.isEmpty()) {
            return text;
        }
    }

    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (!choices.isEmpty() && choices.first().isObject()) {
        const QJsonObject first = choices.first().toObject();
        QString text = first.value(QStringLiteral("text")).toString().trimmed();
        if (!text.isEmpty()) {
            return text;
        }

        text = first.value(QStringLiteral("message")).toObject().value(QStringLiteral("content")).toString().trimmed();
        if (!text.isEmpty()) {
            return text;
        }
    }

    QString fallback = root.value(QStringLiteral("response")).toString().trimmed();
    if (!fallback.isEmpty()) {
        return fallback;
    }
    return root.value(QStringLiteral("content")).toString().trimmed();
}

} // namespace TranslationBackend
