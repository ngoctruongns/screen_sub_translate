#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QQueue>
#include <QString>

#include "source_language.h"
#include "tuning_params.h"

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
    // Switches the stabilization rules to another source language and drops all
    // candidate/dedupe state, which is meaningless across a language change.
    void setLanguage(SourceLanguage language);
    SourceLanguage language() const { return language_; }
    void onSubtitleDisappeared();
    Decision process(const QString &ocrText);

    // How much meaning a source line carries, in language-neutral units: Han characters
    // for Chinese, whitespace-delimited words for English. Public because the display and
    // incomplete-phrase logic in OverlayWindow measures source lines the same way.
    static int contentUnitCount(const QString &text, SourceLanguage language);
    static int countHanChars(const QString &text);
    static int countLatinWords(const QString &text);

private:
    int requiredStableMsForCandidate(const QString &candidate, int baseStableMs) const;
    int requiredSeenFramesForCandidate(const QString &candidate) const;
    int candidateQualityScore(const QString &text) const;
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

    SourceLanguage language_ = sourcelang::kDefault;
    const tuning::LanguageProfile *profile_ = &tuning::profileFor(sourcelang::kDefault);

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
