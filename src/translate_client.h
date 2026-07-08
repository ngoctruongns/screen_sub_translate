#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <QPair>

#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

class TranslateClient : public QObject
{
    Q_OBJECT

public:
    explicit TranslateClient(QObject *parent = nullptr);
    ~TranslateClient() override;

    void requestTranslation(const QString &sourceText);

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
    void startBackendRequest(const QString &sourceText);
    void startBackendPromptRequest(const QString &sourceText,
                                   const QString &prompt,
                                   const std::optional<QString> &draftTranslation,
                                   bool isRepairPass,
                                   bool isRescuePass);
    QString recentDialogueContext() const;
    void rememberTranslationContext(const QString &sourceText, const QString &translatedText);
    QString applyGlossaryAliasNormalization(const QString &translatedText) const;

    QNetworkAccessManager *networkManager_ = nullptr;
    QPointer<QNetworkReply> activeReply_;

    bool localInitialized_ = false;
    QString inFlightText_;
    QString pendingText_;

    QString backendBaseUrl_;
    QString backendModel_;
    QString backendApiMode_;
    QString backendConfigPath_;
    QString promptContextFilePath_;
    QString glossaryFilePath_;
    bool autoDiscoverModel_ = true;
    bool cachePrompt_ = true;
    int repeatLastN_ = 64;
    int modelDiscoveryTimeoutMs_ = 1200;
    double repeatPenalty_ = 1.1;
    double frequencyPenalty_ = 1.05;
    QString cachedContextBlock_;  // Loaded once at init, reused for prefix cache hit
    QVector<QPair<QString, QString>> glossaryAliasPairs_; // alias -> canonical Vietnamese name
    QVector<TranslationContextEntry> recentTranslationHistory_;

};
