#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include "source_language.h"

class SubtitleLogger : public QObject
{
    Q_OBJECT

public:
    explicit SubtitleLogger(const QString &debugLogFilePath,
                            const QString &subtitleLogDirPath,
                            bool debugLogEnabled,
                            SourceLanguage sourceLanguage,
                            QObject *parent = nullptr);

public slots:
    void initialize();
    // Re-points the source-side .srt at the new language's file and starts a fresh
    // session, since the segments collected so far are in the previous language.
    void setSourceLanguage(SourceLanguage sourceLanguage);
    void logDebugEvent(const QString &status, const QString &sourceText,
                       const QString &translatedText);
    void startSubtitle(const QString &sourceText, qint64 startedAtMs);
    void updateTranslation(const QString &sourceText, const QString &translatedText);
    void endSubtitle(qint64 endedAtMs);
    void shutdown(qint64 endedAtMs);

private:
    struct SubtitleSegment {
        QString sourceText;
        QString translatedText;
        qint64 startedAtMs = 0;
        qint64 endedAtMs = 0;
        bool closed = false;
    };

    void closeActiveSegment(qint64 endedAtMs);
    void rewriteSrtFiles() const;
    void rewriteSingleSrtFile(const QString &filePath, bool useTranslatedText) const;
    QString formatDebugTimestamp(qint64 timestampMs) const;
    QString formatSrtTimestamp(qint64 timestampMs) const;
    QString sanitizeInlineText(const QString &text) const;
    QString sanitizeMultilineText(const QString &text) const;

    QString debugLogFilePath_;
    QString subtitleLogDirPath_;
    // Source-side .srt for the active language (chinese.srt / english.srt).
    QString sourceSubtitleLogPath_;
    QString vietnameseSubtitleLogPath_;
    SourceLanguage sourceLanguage_ = sourcelang::kDefault;
    bool debugLogEnabled_ = false;
    qint64 sessionStartedAtMs_ = 0;
    int activeSegmentIndex_ = -1;
    QVector<SubtitleSegment> segments_;
};