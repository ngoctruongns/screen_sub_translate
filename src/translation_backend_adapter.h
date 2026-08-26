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
    // Connection only. Everything that shapes the model's OUTPUT — sampling, penalties,
    // token budget, retry behaviour — lives in config/tuning.json, so there is exactly one
    // file to tune. This file answers "which backend, where, and with which data files".
    QString baseUrl;
    QString model;
    QString apiMode;
    QString glossaryFilePath;    // Chinese-source glossary.
    QString glossaryFilePathEn;  // English-source glossary.
    // Source language the app starts in ("zh" | "en"). Only a default: once the user picks
    // a language from the capture window's context menu, that choice is remembered in
    // QSettings and wins over this field.
    QString sourceLanguage;
    bool autoDiscoverModel = true;
    int modelDiscoveryTimeoutMs = 1200;
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
