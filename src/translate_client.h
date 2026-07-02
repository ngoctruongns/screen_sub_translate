#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

class TranslateClient : public QObject
{
    Q_OBJECT

public:
    enum class Backend {
        Local,
        GoogleApi,
    };

    explicit TranslateClient(QObject *parent = nullptr);
    ~TranslateClient() override;

    void setBackend(Backend backend);
    Backend backend() const;
    void requestTranslation(const QString &sourceText);

signals:
    void translationReady(const QString &translatedText, const QString &sourceText);
    void translationError(const QString &error);
    void backendChanged(const QString &backendName);

private slots:
    void onReplyFinished(QNetworkReply *reply);

private:
    struct TranslationContextEntry {
        QString sourceText;
        QString translatedText;
    };

    void initializeLocalBackend();
    void startLlamaRequest(const QString &sourceText);
    void startGoogleRequest(const QString &sourceText);
    void startLlamaPromptRequest(const QString &sourceText,
                                 const QString &prompt,
                                 const std::optional<QString> &draftTranslation,
                                 bool isRepairPass);
    QString recentDialogueContext() const;
    void rememberTranslationContext(const QString &sourceText, const QString &translatedText);

    Backend backend_ = Backend::Local;

    QNetworkAccessManager *networkManager_ = nullptr;
    QPointer<QNetworkReply> activeReply_;

    bool localInitialized_ = false;
    QString inFlightText_;
    QString pendingText_;

    QString llamaBaseUrl_;
    QString llamaModel_;
    QString promptContextFilePath_;
    QVector<TranslationContextEntry> recentTranslationHistory_;

};
