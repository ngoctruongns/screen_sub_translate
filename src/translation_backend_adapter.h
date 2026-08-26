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
    QString glossaryFilePath;    // Chinese-source glossary.
    QString glossaryFilePathEn;  // English-source glossary.
    // Source language the app starts in ("zh" | "en"). Only a default: once the user picks
    // a language from the capture window's context menu, that choice is remembered in
    // QSettings and wins over this field.
    QString sourceLanguage;
    bool autoDiscoverModel = true;
    bool cachePrompt = true;
    int modelDiscoveryTimeoutMs = 1200;

    // ── Generation ────────────────────────────────────────────────────────
    double temperature = 0.01;      // First-pass sampling temperature. 0.0 = greedy; keep low for faithful subtitles.
    int numPredict = 64;            // Max new tokens per response (~64 covers most subtitle lines).
    double retryTemperature = 0.3;  // Retry-pass temperature. Deliberately looser than first pass so the model can
                                    // escape a degenerate first output (repetition / copied Han) instead of reproducing it.

    // ── Repetition penalties ──────────────────────────────────────────────
    // Applied over the last repeatLastN tokens; discourages repeated phrases
    // and multi-candidate looping that instruction-tuned models sometimes produce.
    int repeatLastN = 128;          // Token look-back window for repeat_penalty.
                                    // Higher → penalises longer-range repetition.
    double repeatPenalty = 1.15;    // Multiplier on tokens already seen in the window.
                                    // 1.0 = disabled; 1.1–1.2 is typical for chat models.
    double frequencyPenalty = 1.15; // Additional penalty scaling with how often the token
                                    // has appeared so far. Reduces phrase-level repetition.

    // ── Sampling parameters ───────────────────────────────────────────────
    // These three filters are applied in order (topK → topP → minP) before
    // the final token is sampled from the remaining distribution.
    int topK = 40;      // Retain only the top-K most probable tokens before sampling.
                        // Lower → more conservative vocabulary. 0 = disabled.
    double topP = 0.85; // Nucleus sampling: keep the smallest set of tokens whose
                        // cumulative probability ≥ topP. 0.8–0.95 is typical.
    double minP = 0.06; // Minimum probability filter: discard any token whose probability
                        // is < minP × (best token probability at this step).
                        // Particularly effective at suppressing low-probability Han characters
                        // that can leak into Vietnamese output (set 0.05–0.10).
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
