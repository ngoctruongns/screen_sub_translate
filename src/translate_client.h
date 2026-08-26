#pragma once

#include <optional>

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <QPair>

#include "source_language.h"
#include "translation_text_processor.h"
#include "tuning_params.h"

class QNetworkAccessManager;
class QNetworkReply;

class TranslateClient : public QObject
{
    Q_OBJECT

public:
    explicit TranslateClient(QObject *parent = nullptr);
    ~TranslateClient() override;

    void requestTranslation(const QString &sourceText);

    // Switches the source language: reloads that language's glossary and drops the
    // translation cache and dialogue history, both of which are keyed by source lines
    // that no longer apply. The startup value comes from translation_backend.json;
    // OverlayWindow overrides it with the user's last UI choice.
    void setSourceLanguage(SourceLanguage language);
    SourceLanguage sourceLanguage() const { return sourceLanguage_; }
    // The language the backend config asked for, used as the startup default.
    SourceLanguage configuredSourceLanguage() const { return configuredSourceLanguage_; }

signals:
    void translationReady(const QString &translatedText, const QString &sourceText);
    void translationError(const QString &error);

private slots:
    void onReplyFinished(QNetworkReply *reply);

private:
    struct TranslationContextEntry {
        QString sourceText;
        QString translatedText;
    };

    void initializeTranslationBackend();
    // (Re)loads the glossary + output-side aliases for the active source language.
    void loadGlossaryForCurrentLanguage();
    void startBackendRequest(const QString &sourceText);
    void startBackendPromptRequest(const QString &sourceText, const QString &prompt, bool isRetryPass,
                                   const QString &priorSalvage = QString());
    // Last resort when a pass fails the quality gate and no (further) retry is possible:
    // recover the best Vietnamese fragment from the raw output (or the earlier pass's
    // salvage) and emit it, so the subtitle degrades instead of vanishing. Emits
    // translationError only when nothing usable can be recovered.
    void emitBestEffortOrError(const QString &sourceText, const QString &rawTranslated,
                               const QString &priorSalvage,
                               TranslationTextProcessor::TranslationIssue issue);
    QString recentDialogueContext() const;
    void rememberTranslationContext(const QString &sourceText, const QString &translatedText);
    QString applyGlossaryAliasNormalization(const QString &translatedText) const;

    // Bounded LRU cache of successful translations, keyed by the trimmed source line.
    // Repeated subtitles (very common in film dialogue) are served instantly without a
    // backend round-trip. Order list front = least-recently-used.
    std::optional<QString> lookupTranslationCache(const QString &sourceText);
    void insertTranslationCache(const QString &sourceText, const QString &translatedText);

    QNetworkAccessManager *networkManager_ = nullptr;
    QPointer<QNetworkReply> activeReply_;

    bool localInitialized_ = false;
    QString inFlightText_;
    QString pendingText_;

    QString backendBaseUrl_;
    QString backendModel_;
    QString backendApiMode_;
    QString backendConfigPath_;
    // Glossary file per source language; the active one is selected by sourceLanguage_.
    QString glossaryFilePathZh_;
    QString glossaryFilePathEn_;

    SourceLanguage sourceLanguage_ = sourcelang::kDefault;
    SourceLanguage configuredSourceLanguage_ = sourcelang::kDefault;
    bool autoDiscoverModel_ = true;
    int modelDiscoveryTimeoutMs_ = 1200;

    // Sampling / penalty / token-budget knobs are NOT cached here: they live in
    // config/tuning.json, are applied to the tuning:: globals at startup, and are read
    // directly at request time. Caching them would add a second source of truth.
    QVector<QPair<QString, QString>> glossaryPairs_; // source term (Han or English) -> canonical Vietnamese term
    QVector<QPair<QString, QString>> aliasPairs_; // alias -> canonical Vietnamese name
    QVector<TranslationContextEntry> recentTranslationHistory_;

    QHash<QString, QString> translationCache_;   // trimmed source -> final Vietnamese
    QList<QString> translationCacheOrder_;        // LRU order (front = oldest)

};
