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

    void initializeLocalBackend();
    void startLlamaRequest(const QString &sourceText);
    void startLlamaPromptRequest(const QString &sourceText,
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

    QString llamaBaseUrl_;
    QString llamaModel_;
    QString llmApiMode_;
    QString promptContextFilePath_;
    QString glossaryFilePath_;
    QString cachedContextBlock_;  // Loaded once at init, reused for prefix cache hit
    QVector<QPair<QString, QString>> glossaryAliasPairs_; // alias -> canonical Vietnamese name
    QVector<TranslationContextEntry> recentTranslationHistory_;

};
