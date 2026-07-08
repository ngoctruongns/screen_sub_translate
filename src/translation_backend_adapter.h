#pragma once

#include <optional>

#include <QJsonObject>
#include <QString>
#include <QUrl>

namespace TranslationBackend
{

enum class ApiMode {
    Auto,
    LlamaCpp,
    OpenAI,
    Ollama,
};

struct BackendConfig {
    QString baseUrl;
    QString model;
    QString apiMode;
    QString contextFilePath;
    QString glossaryFilePath;
    bool autoDiscoverModel = true;
    bool cachePrompt = true;
    int repeatLastN = 64;
    int modelDiscoveryTimeoutMs = 1200;
    double repeatPenalty = 1.1;
    double frequencyPenalty = 1.05;
};

// Config loading
BackendConfig loadBackendConfig(const QString &configPath);
BackendConfig defaultBackendConfig();

// API mode resolution
ApiMode parseApiMode(const QString &modeText);
ApiMode resolveApiMode(const QString &modeText, const QString &baseUrl);
QString apiModeName(ApiMode mode);

// Endpoint URL builders
QUrl endpointUrl(const QString &baseUrl, ApiMode mode);
QUrl modelDiscoveryUrl(const QString &baseUrl, ApiMode mode);

// Model discovery (synchronous, blocking)
std::optional<QString> discoverModel(const QString &baseUrl, ApiMode mode, int timeoutMs);

// Response parsing
QString extractResponseText(const QJsonObject &root, ApiMode mode);

} // namespace TranslationBackend
