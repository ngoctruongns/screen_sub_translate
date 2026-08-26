#include "subtitle_logger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <algorithm>

#include "tuning_params.h"

namespace
{
QString sourceSrtPathFor(const QString &subtitleLogDirPath, SourceLanguage language)
{
    return QDir(subtitleLogDirPath)
        .filePath(tuning::profileFor(language).sourceSrtFileName);
}
} // namespace

SubtitleLogger::SubtitleLogger(const QString &debugLogFilePath,
                               const QString &subtitleLogDirPath,
                               bool debugLogEnabled,
                               SourceLanguage sourceLanguage,
                               QObject *parent)
    : QObject(parent),
      debugLogFilePath_(debugLogFilePath),
      subtitleLogDirPath_(subtitleLogDirPath),
      sourceSubtitleLogPath_(sourceSrtPathFor(subtitleLogDirPath, sourceLanguage)),
      vietnameseSubtitleLogPath_(QDir(subtitleLogDirPath).filePath(QStringLiteral("vietnamese.srt"))),
      sourceLanguage_(sourceLanguage),
      debugLogEnabled_(debugLogEnabled)
{
}

void SubtitleLogger::setSourceLanguage(SourceLanguage sourceLanguage)
{
    if (sourceLanguage == sourceLanguage_) {
        return;
    }

    // Close out whatever is still open under the old language before switching files,
    // so the previous .srt is left consistent rather than missing its last segment.
    closeActiveSegment(QDateTime::currentMSecsSinceEpoch());
    rewriteSrtFiles();

    sourceLanguage_ = sourceLanguage;
    sourceSubtitleLogPath_ = sourceSrtPathFor(subtitleLogDirPath_, sourceLanguage);

    // Segments collected so far are in the previous language and must not be rewritten
    // into the new language's file; initialize() drops them and truncates both files.
    initialize();
}

void SubtitleLogger::initialize()
{
    sessionStartedAtMs_ = QDateTime::currentMSecsSinceEpoch();
    segments_.clear();
    activeSegmentIndex_ = -1;

    QDir subtitleDir;
    subtitleDir.mkpath(subtitleLogDirPath_);

    QFile sourceFile(sourceSubtitleLogPath_);
    if (sourceFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        sourceFile.resize(0);
        sourceFile.close();
    }

    QFile vietnameseFile(vietnameseSubtitleLogPath_);
    if (vietnameseFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        vietnameseFile.resize(0);
        vietnameseFile.close();
    }

    if (debugLogEnabled_) {
        QFileInfo debugInfo(debugLogFilePath_);
        QDir().mkpath(debugInfo.absolutePath());

        QFile debugFile(debugLogFilePath_);
        if (debugFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            debugFile.resize(0);
            debugFile.close();
        }
    }
}

void SubtitleLogger::logDebugEvent(const QString &status, const QString &sourceText,
                                   const QString &translatedText)
{
    if (!debugLogEnabled_) {
        return;
    }

    QFile file(debugLogFilePath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    QTextStream out(&file);
    out << '[' << formatDebugTimestamp(QDateTime::currentMSecsSinceEpoch()) << "] "
        << status << " | source=" << sanitizeInlineText(sourceText)
        << " | result=" << sanitizeInlineText(translatedText) << '\n';
}

void SubtitleLogger::startSubtitle(const QString &sourceText, qint64 startedAtMs)
{
    if (sourceText.isEmpty()) {
        return;
    }

    closeActiveSegment(startedAtMs);

    SubtitleSegment segment;
    segment.sourceText = sourceText;
    segment.startedAtMs = std::max(startedAtMs, sessionStartedAtMs_);
    segments_.append(segment);
    activeSegmentIndex_ = segments_.size() - 1;
    rewriteSrtFiles();
}

void SubtitleLogger::updateTranslation(const QString &sourceText, const QString &translatedText)
{
    if (sourceText.isEmpty() || translatedText.isEmpty()) {
        return;
    }

    for (int index = segments_.size() - 1; index >= 0; --index) {
        SubtitleSegment &segment = segments_[index];
        if (segment.sourceText != sourceText) {
            continue;
        }

        if (segment.translatedText == translatedText) {
            return;
        }

        segment.translatedText = translatedText;
        rewriteSrtFiles();
        return;
    }
}

void SubtitleLogger::endSubtitle(qint64 endedAtMs)
{
    closeActiveSegment(endedAtMs);
    rewriteSrtFiles();
}

void SubtitleLogger::shutdown(qint64 endedAtMs)
{
    closeActiveSegment(endedAtMs);
    rewriteSrtFiles();
}

void SubtitleLogger::closeActiveSegment(qint64 endedAtMs)
{
    if (activeSegmentIndex_ < 0 || activeSegmentIndex_ >= segments_.size()) {
        activeSegmentIndex_ = -1;
        return;
    }

    SubtitleSegment &segment = segments_[activeSegmentIndex_];
    segment.endedAtMs = std::max(endedAtMs, segment.startedAtMs);
    segment.closed = true;
    activeSegmentIndex_ = -1;
}

void SubtitleLogger::rewriteSrtFiles() const
{
    rewriteSingleSrtFile(sourceSubtitleLogPath_, false);
    rewriteSingleSrtFile(vietnameseSubtitleLogPath_, true);
}

void SubtitleLogger::rewriteSingleSrtFile(const QString &filePath, bool useTranslatedText) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }

    QTextStream out(&file);
    int sequence = 1;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    for (const SubtitleSegment &segment : segments_) {
        const QString text = useTranslatedText ? segment.translatedText : segment.sourceText;
        if (text.isEmpty()) {
            continue;
        }

        const qint64 endedAtMs = segment.closed ? segment.endedAtMs : std::max(now, segment.startedAtMs);
        out << sequence++ << '\n';
        out << formatSrtTimestamp(segment.startedAtMs) << " --> "
            << formatSrtTimestamp(endedAtMs) << '\n';
        out << sanitizeMultilineText(text) << "\n\n";
    }
}

QString SubtitleLogger::formatDebugTimestamp(qint64 timestampMs) const
{
    return QDateTime::fromMSecsSinceEpoch(timestampMs)
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
}

QString SubtitleLogger::formatSrtTimestamp(qint64 timestampMs) const
{
    const qint64 elapsedMs = std::max<qint64>(0, timestampMs - sessionStartedAtMs_);
    const qint64 hours = elapsedMs / 3600000;
    const qint64 minutes = (elapsedMs % 3600000) / 60000;
    const qint64 seconds = (elapsedMs % 60000) / 1000;
    const qint64 milliseconds = elapsedMs % 1000;

    return QStringLiteral("%1:%2:%3,%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(milliseconds, 3, 10, QLatin1Char('0'));
}

QString SubtitleLogger::sanitizeInlineText(const QString &text) const
{
    QString sanitized = text;
    sanitized.replace('\r', QLatin1Char(' '));
    sanitized.replace('\n', QLatin1Char(' '));
    return sanitized;
}

QString SubtitleLogger::sanitizeMultilineText(const QString &text) const
{
    QString sanitized = text;
    sanitized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    sanitized.replace('\r', '\n');
    return sanitized.trimmed();
}