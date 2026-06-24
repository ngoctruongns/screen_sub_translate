#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>
#include <string>
#include <unordered_map>

#include <onnxruntime_cxx_api.h>
#include <sentencepiece_processor.h>

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
    void initializeLocalBackend();
    void startGoogleRequest(const QString &sourceText);
    void startInference(const QString &sourceText);

    Backend backend_ = Backend::Local;

    QNetworkAccessManager *networkManager_ = nullptr;
    QPointer<QNetworkReply> activeReply_;

    Ort::Env env_;
    Ort::SessionOptions sessionOptions_;
    std::unique_ptr<Ort::Session> encoderSession_;
    std::unique_ptr<Ort::Session> decoderSession_;

    std::unique_ptr<sentencepiece::SentencePieceProcessor> srcSP_;
    std::unique_ptr<sentencepiece::SentencePieceProcessor> tgtSP_;

    // vocab.json: piece string <-> model token ID (shared vocab, 65001 entries)
    std::unordered_map<std::string, int64_t> vocabMap_;
    std::unordered_map<int64_t, std::string> idToVocab_;

    bool localInitialized_ = false;
    bool busy_ = false;
    QString inFlightText_;
    QString pendingText_;

    QFutureWatcher<QString> *watcher_ = nullptr;
};
