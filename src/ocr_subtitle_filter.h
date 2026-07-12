#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QQueue>
#include <QString>

class OcrSubtitleFilter
{
public:
    struct Decision {
        bool rejectedForQuality = false;
        bool rolledBack = false;
        bool shouldDispatch = false;
        QString rollbackBestText;
        QString dispatchText;
        int stableElapsedMs = 0;
        int seenFrames = 0;
    };

    void configure(int minOcrLength, int minCandidateStableMs);
    void onSubtitleDisappeared();
    Decision process(const QString &ocrText);

private:
    static int countHanChars(const QString &text);
    static int requiredStableMsForCandidate(const QString &candidate, int baseStableMs);
    static int requiredSeenFramesForCandidate(const QString &candidate);
    static int candidateQualityScore(const QString &text);
    static int longestCommonSubstring(const QString &left, const QString &right);
    static int longestCommonSubsequence(const QString &left, const QString &right);
    static int levenshteinDistance(const QString &left, const QString &right);

    bool isLikelySameSubtitle(const QString &left, const QString &right) const;
    bool shouldDispatchSubtitle(const QString &ocrText);
    bool wasRecentlyDispatched(const QString &key) const;
    void rememberDispatchedSubtitle(const QString &key);
    QString mostFrequentCandidate() const;
    QString subtitleKey(const QString &text) const;
    void resetCandidateState();

    int minOcrLength_ = 0;
    int minCandidateStableMs_ = 0;

    QString candidateText_;
    QElapsedTimer candidateTimer_;
    int candidateSeenFrames_ = 0;
    QHash<QString, int> candidateFrequency_;

    bool subtitleVisible_ = false;
    QString lastDispatchedSubtitleKey_;
    QElapsedTimer subtitleDispatchTimer_;
    QQueue<QString> recentSubtitleKeys_;
};
