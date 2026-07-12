#include "ocr_subtitle_filter.h"

#include <algorithm>

#include <QVector>

#include "tuning_params.h"

void OcrSubtitleFilter::configure(int minOcrLength, int minCandidateStableMs)
{
    minOcrLength_ = minOcrLength;
    minCandidateStableMs_ = minCandidateStableMs;
    resetCandidateState();
}

void OcrSubtitleFilter::onSubtitleDisappeared()
{
    subtitleVisible_ = false;
    recentSubtitleKeys_.clear();
    resetCandidateState();
}

OcrSubtitleFilter::Decision OcrSubtitleFilter::process(const QString &ocrText)
{
    Decision decision;
    if (ocrText.isEmpty()) {
        return decision;
    }

    if (countHanChars(ocrText) < tuning::kMinHanCharsForCandidate) {
        resetCandidateState();
        decision.rejectedForQuality = true;
        return decision;
    }

    if (!isLikelySameSubtitle(ocrText, candidateText_)) {
        const QString bestText = mostFrequentCandidate();

        const int minLen = std::min(ocrText.size(), bestText.size());
        const int maxLen = std::max(ocrText.size(), bestText.size());
        const bool hasContainment =
            !bestText.isEmpty() && (ocrText.contains(bestText) || bestText.contains(ocrText));
        const bool isPartialRead = hasContainment && minLen >= 2 && (maxLen - minLen) <= 2;

        if (isPartialRead) {
            candidateFrequency_[ocrText]++;
            candidateText_ = (candidateQualityScore(ocrText) >= candidateQualityScore(bestText))
                                 ? ocrText
                                 : bestText;
            decision.rolledBack = true;
            decision.rollbackBestText = candidateText_;
        } else {
            candidateFrequency_.clear();
            candidateFrequency_[ocrText] = 1;
            candidateText_ = ocrText;
            candidateTimer_.restart();
            candidateSeenFrames_ = 1;
        }
    } else {
        if (!candidateTimer_.isValid()) {
            candidateTimer_.restart();
        }
        candidateFrequency_[ocrText]++;
        ++candidateSeenFrames_;
        candidateText_ = mostFrequentCandidate();
    }

    const bool enoughLength = candidateText_.size() >= minOcrLength_;
    const int requiredStableMs = requiredStableMsForCandidate(candidateText_, minCandidateStableMs_);
    const int requiredFrames = requiredSeenFramesForCandidate(candidateText_);
    const bool stableEnough = candidateTimer_.isValid() && candidateTimer_.elapsed() >= requiredStableMs;
    const bool seenEnoughFrames = candidateSeenFrames_ >= requiredFrames;

    if (enoughLength && stableEnough && seenEnoughFrames && shouldDispatchSubtitle(candidateText_)) {
        decision.shouldDispatch = true;
        decision.dispatchText = candidateText_;
        decision.stableElapsedMs = candidateTimer_.isValid() ? candidateTimer_.elapsed() : 0;
        decision.seenFrames = candidateSeenFrames_;
    }

    return decision;
}

int OcrSubtitleFilter::countHanChars(const QString &text)
{
    int count = 0;
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        const bool isHan = (u >= 0x3400 && u <= 0x4DBF) || (u >= 0x4E00 && u <= 0x9FFF) ||
                           (u >= 0xF900 && u <= 0xFAFF);
        if (isHan) {
            ++count;
        }
    }
    return count;
}

int OcrSubtitleFilter::requiredStableMsForCandidate(const QString &candidate, int baseStableMs)
{
    if (candidate.size() <= 1) {
        return std::max(baseStableMs, tuning::kVeryShortCandidateStableMs);
    }

    if (candidate.size() <= 3) {
        return std::max(baseStableMs, tuning::kShortCandidateStableMs);
    }

    return baseStableMs;
}

int OcrSubtitleFilter::requiredSeenFramesForCandidate(const QString &candidate)
{
    if (candidate.size() <= 1) {
        return tuning::kVeryShortCandidateMinFrames;
    }

    if (candidate.size() <= 3) {
        return tuning::kShortCandidateMinFrames;
    }

    return 1;
}

int OcrSubtitleFilter::candidateQualityScore(const QString &text)
{
    return countHanChars(text) * 4 + text.size();
}

int OcrSubtitleFilter::longestCommonSubstring(const QString &left, const QString &right)
{
    const int n = left.size();
    const int m = right.size();

    int maxLen = 0;
    QVector<int> prev(m + 1, 0);
    QVector<int> curr(m + 1, 0);

    for (int i = 1; i <= n; ++i) {
        for (int j = 1; j <= m; ++j) {
            if (left.at(i - 1) == right.at(j - 1)) {
                curr[j] = prev[j - 1] + 1;
                maxLen = std::max(maxLen, curr[j]);
            } else {
                curr[j] = 0;
            }
        }
        std::swap(prev, curr);
        std::fill(curr.begin(), curr.end(), 0);
    }

    return maxLen;
}

int OcrSubtitleFilter::longestCommonSubsequence(const QString &left, const QString &right)
{
    if (left.size() < right.size()) {
        return longestCommonSubsequence(right, left);
    }

    const int n = left.size();
    const int m = right.size();

    QVector<int> dp(m + 1, 0);

    for (int i = 1; i <= n; ++i) {
        int prev = 0;
        for (int j = 1; j <= m; ++j) {
            int temp = dp[j];
            if (left.at(i - 1) == right.at(j - 1)) {
                dp[j] = prev + 1;
            } else {
                dp[j] = std::max(dp[j], dp[j - 1]);
            }
            prev = temp;
        }
    }

    return dp[m];
}

int OcrSubtitleFilter::levenshteinDistance(const QString &left, const QString &right)
{
    const int n = left.size();
    const int m = right.size();

    if (n == 0) {
        return m;
    }
    if (m == 0) {
        return n;
    }

    QVector<int> prev(m + 1);
    QVector<int> curr(m + 1);

    for (int j = 0; j <= m; ++j) {
        prev[j] = j;
    }

    for (int i = 1; i <= n; ++i) {
        curr[0] = i;

        for (int j = 1; j <= m; ++j) {
            const int cost = (left.at(i - 1) == right.at(j - 1)) ? 0 : 1;

            curr[j] = std::min({
                prev[j] + 1,
                curr[j - 1] + 1,
                prev[j - 1] + cost,
            });
        }

        std::swap(prev, curr);
    }

    return prev[m];
}

bool OcrSubtitleFilter::isLikelySameSubtitle(const QString &left, const QString &right) const
{
    if (left.isEmpty() || right.isEmpty()) {
        return false;
    }

    if (left == right) {
        return true;
    }

    const int minLen = std::min(left.size(), right.size());
    const int maxLen = std::max(left.size(), right.size());
    const int lenDiff = std::abs(left.size() - right.size());

    if (lenDiff > 3) {
        return false;
    }

    if (minLen >= 2 && lenDiff <= 2) {
        const QString &shorter = (left.size() < right.size()) ? left : right;
        const QString &longer = (left.size() < right.size()) ? right : left;

        if (longer.contains(shorter)) {
            return true;
        }
    }

    if ((left.contains(right) || right.contains(left)) && minLen >= 6 && lenDiff <= 1) {
        return true;
    }

    const int lev = levenshteinDistance(left, right);

    if (minLen <= 1) {
        return lev == 0;
    }

    if (minLen <= 3) {
        return lev <= 1;
    }

    if (lev <= 1) {
        return true;
    }

    const int lcsSub = longestCommonSubstring(left, right);
    if (lcsSub >= 3 && static_cast<double>(lcsSub) / minLen >= 0.60) {
        return true;
    }

    if (minLen <= 4) {
        return false;
    }

    const int lcs = longestCommonSubsequence(left, right);
    const double lcsRatio = static_cast<double>(lcs) / static_cast<double>(std::max(1, maxLen));
    const double levRatio = static_cast<double>(lev) / static_cast<double>(std::max(1, maxLen));

    return (lcsRatio >= 0.82) && (levRatio <= 0.24);
}

bool OcrSubtitleFilter::shouldDispatchSubtitle(const QString &ocrText)
{
    const QString key = subtitleKey(ocrText);
    if (key.isEmpty()) {
        return false;
    }

    if (!subtitleVisible_) {
        subtitleVisible_ = true;
        lastDispatchedSubtitleKey_ = key;
        rememberDispatchedSubtitle(key);
        subtitleDispatchTimer_.restart();
        return true;
    }

    if (wasRecentlyDispatched(key)) {
        return false;
    }

    if (subtitleDispatchTimer_.isValid() &&
        subtitleDispatchTimer_.elapsed() < tuning::kSubtitleSwitchCooldownMs) {
        return false;
    }

    const bool isRepeatKey = (key == lastDispatchedSubtitleKey_);
    if (isRepeatKey && subtitleDispatchTimer_.isValid() &&
        subtitleDispatchTimer_.elapsed() < tuning::kSubtitleResendCooldownMs) {
        return false;
    }

    lastDispatchedSubtitleKey_ = key;
    rememberDispatchedSubtitle(key);
    subtitleDispatchTimer_.restart();

    return true;
}

bool OcrSubtitleFilter::wasRecentlyDispatched(const QString &key) const
{
    for (const QString &oldKey : recentSubtitleKeys_) {
        if (isLikelySameSubtitle(key, oldKey)) {
            return true;
        }
    }

    return false;
}

void OcrSubtitleFilter::rememberDispatchedSubtitle(const QString &key)
{
    recentSubtitleKeys_.enqueue(key);

    while (recentSubtitleKeys_.size() > tuning::kRecentSubtitleWindowSize) {
        recentSubtitleKeys_.dequeue();
    }
}

QString OcrSubtitleFilter::mostFrequentCandidate() const
{
    QString best = candidateText_;
    int bestCount = -1;
    int bestQuality = candidateQualityScore(best);
    for (auto it = candidateFrequency_.cbegin(); it != candidateFrequency_.cend(); ++it) {
        const QString candidate = it.key();
        const int count = it.value();
        const int quality = candidateQualityScore(candidate);
        if (count > bestCount ||
            (count == bestCount && quality > bestQuality) ||
            (count == bestCount && quality == bestQuality && candidate.size() > best.size())) {
            best = candidate;
            bestCount = count;
            bestQuality = quality;
        }
    }
    return best;
}

QString OcrSubtitleFilter::subtitleKey(const QString &text) const
{
    QString key;
    key.reserve(text.size());
    for (const QChar c : text) {
        const ushort u = c.unicode();
        const bool isHan = (u >= 0x3400 && u <= 0x9FFF) || (u >= 0xF900 && u <= 0xFAFF);
        const bool isAsciiDigit = (u >= '0' && u <= '9');
        if (isHan || isAsciiDigit) {
            key.append(c);
        }
    }
    return key;
}

void OcrSubtitleFilter::resetCandidateState()
{
    candidateText_.clear();
    candidateTimer_.invalidate();
    candidateSeenFrames_ = 0;
    candidateFrequency_.clear();
}
