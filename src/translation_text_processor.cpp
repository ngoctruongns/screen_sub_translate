#include "translation_text_processor.h"

#include <algorithm>

#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include "tuning_params.h"

namespace TranslationTextProcessor
{

namespace {  // Anonymous namespace for internal helper functions

bool isHanChar(const QChar &ch)
{
    return ch.script() == QChar::Script_Han;
}

bool isEnglishChar(const QChar &ch)
{
    const ushort u = ch.unicode();
    return (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z');
}

bool isVietnameseAccentChar(const QChar &ch)
{
    const ushort u = ch.unicode();
    // For as Â, Ô, Ơ, Đ ...
    return (u >= 0x00C0 && u <= 0x024F) || (u >= 0x1EA0 && u <= 0x1EF9);
}

bool hasLatinLetter(const QString &text)
{
    for (const QChar ch : text) {
        if (isEnglishChar(ch)) {
            return true;
        }
    }
    return false;
}

bool isWordBoundaryChar(const QChar ch)
{
    return !ch.isLetterOrNumber() && ch != QChar('_');
}

bool containsLatinWholeWordCaseInsensitive(const QString &text, const QString &needle)
{
    if (text.isEmpty() || needle.isEmpty()) {
        return false;
    }

    const QString loweredText = text.toLower();
    const QString loweredNeedle = needle.toLower();
    int searchFrom = 0;

    while (searchFrom < loweredText.size()) {
        const int idx = loweredText.indexOf(loweredNeedle, searchFrom);
        if (idx < 0) {
            return false;
        }

        const int end = idx + loweredNeedle.size();
        const bool leftOk = idx == 0 || isWordBoundaryChar(text.at(idx - 1));
        const bool rightOk = end >= text.size() || isWordBoundaryChar(text.at(end));
        if (leftOk && rightOk) {
            return true;
        }

        searchFrom = idx + loweredNeedle.size();
    }

    return false;
}

int hanCharCount(const QString &text)
{
    int count = 0;
    for (const QChar ch : text) {
        if (isHanChar(ch)) {
            ++count;
        }
    }
    return count;
}

int englishCharCount(const QString &text)
{
    int count = 0;

    for (const QChar ch : text) {
        if (isEnglishChar(ch)) {
            ++count;
        }
    }

    return count;
}

int latinWordCount(const QString &text)
{
    static const QRegularExpression kWordRegex(QStringLiteral("[A-Za-zÀ-ỹĐđ]+"));
    int count = 0;
    auto it = kWordRegex.globalMatch(text);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

int vietnameseAccentCount(const QString &text)
{
    int count = 0;

    for (const QChar ch : text) {
        if (isVietnameseAccentChar(ch)) {
            ++count;
        }
    }

    return count;
}

bool hasVietnameseDiacritic(const QString &text)
{
    for (const QChar ch : text) {
        if (isVietnameseAccentChar(ch)) {
            return true;
        }
    }
    return false;
}

bool isVowelBaseChar(const QChar ch)
{
    const ushort u = ch.unicode();
    return u == 'a' || u == 'e' || u == 'i' || u == 'o' || u == 'u' || u == 'y';
}

// Strip Vietnamese diacritics/tones down to the bare Latin skeleton (đ -> d).
// Lets us test a token against Vietnamese syllable structure regardless of tone marks.
QString toVietnameseBaseLetters(const QString &token)
{
    const QString decomposed = token.toLower().normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar ch : decomposed) {
        if (ch.category() == QChar::Mark_NonSpacing) {
            continue; // Drop combining tone/diacritic marks.
        }
        if (ch == QChar(0x0111)) { // đ (does not decompose)
            out.append(QLatin1Char('d'));
            continue;
        }
        out.append(ch);
    }
    return out;
}

// Heuristic: does a diacritic-stripped token fit Vietnamese syllable structure
// (onset + vowel nucleus + optional coda)? Multi-syllable English words such as
// "prostration"/"surprising"/"however" fail this, while Vietnamese syllables
// written without tone marks ("chung", "khong", "truong") pass.
bool isPlausibleVietnameseSyllable(const QString &base)
{
    if (base.isEmpty()) {
        return false;
    }

    static const QSet<QString> onset3 = {QStringLiteral("ngh")};
    static const QSet<QString> onset2 = {
        QStringLiteral("ch"), QStringLiteral("gh"), QStringLiteral("gi"),
        QStringLiteral("kh"), QStringLiteral("ng"), QStringLiteral("nh"),
        QStringLiteral("ph"), QStringLiteral("qu"), QStringLiteral("th"),
        QStringLiteral("tr")};
    static const QString onset1 = QStringLiteral("bcdghklmnpqrstvx");
    static const QSet<QString> coda = {
        QStringLiteral("c"), QStringLiteral("ch"), QStringLiteral("m"),
        QStringLiteral("n"), QStringLiteral("ng"), QStringLiteral("nh"),
        QStringLiteral("p"), QStringLiteral("t"),
        QStringLiteral("i"), QStringLiteral("o"), QStringLiteral("u"), QStringLiteral("y")};

    const int n = base.size();
    int pos = 0;

    // Onset: longest match wins (ngh > ng > n).
    if (n - pos >= 3 && onset3.contains(base.mid(pos, 3))) {
        pos += 3;
    } else if (n - pos >= 2 && onset2.contains(base.mid(pos, 2))) {
        pos += 2;
    } else if (n - pos >= 1 && onset1.contains(base.at(pos))) {
        pos += 1;
    }

    // Nucleus: at least one vowel.
    const int vowelStart = pos;
    while (pos < n && isVowelBaseChar(base.at(pos))) {
        ++pos;
    }
    if (pos == vowelStart) {
        return false;
    }

    // Coda: optional, must be a valid final cluster.
    if (pos == n) {
        return true;
    }
    return coda.contains(base.mid(pos));
}

// Count the isolated non-Vietnamese Latin words leaking into the output
// (e.g. "prostration", "surprising"). Capitalized tokens are treated as proper
// names (allowed by the rules) and skipped; tokens carrying Vietnamese diacritics
// are certainly Vietnamese and skipped. High precision on purpose: short tokens
// that coincide with a Vietnamese syllable shape are left alone.
//
// The English pipeline needs the count rather than a yes/no, because it grades a line
// by how much of it survived untranslated: a couple of leftovers is a repairable
// residue, a majority means nothing was translated at all.
int foreignLatinWordCount(const QString &text)
{
    static const QRegularExpression kToken(QStringLiteral("[A-Za-zÀ-ỹĐđ]+"));
    int count = 0;
    auto it = kToken.globalMatch(text);
    while (it.hasNext()) {
        const QString token = it.next().captured(0);
        if (token.size() < 3) {
            continue;
        }
        if (token.at(0).isUpper()) {
            continue; // Likely a proper name.
        }
        if (hasVietnameseDiacritic(token)) {
            continue; // Certainly Vietnamese.
        }
        if (!isPlausibleVietnameseSyllable(toVietnameseBaseLetters(token))) {
            ++count;
        }
    }
    return count;
}

} // anonymous namespace (resumed below) *********************************************************

// Drop isolated non-Vietnamese Latin words (e.g. "prostration", "stupefied") from a
// line while leaving Vietnamese syllables, proper names, and numbers intact. Used by
// salvage so an otherwise-Vietnamese line with a couple of English leftovers can still
// be recovered, and by the English pipeline's ResidualEnglish repair path as the
// counterpart of removeHanCharacters(). Uses the same high-precision test as
// foreignLatinWordCount().
QString removeForeignLatinWords(QString text)
{
    static const QRegularExpression kToken(QStringLiteral("[A-Za-zÀ-ỹĐđ]+"));
    QVector<QPair<int, int>> removals; // (start, length), collected in order.
    auto it = kToken.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString token = match.captured(0);
        if (token.size() < 3 || token.at(0).isUpper() || hasVietnameseDiacritic(token)) {
            continue;
        }
        if (!isPlausibleVietnameseSyllable(toVietnameseBaseLetters(token))) {
            removals.append({match.capturedStart(0), match.capturedLength(0)});
        }
    }

    // Remove from the tail so earlier offsets stay valid.
    for (int i = removals.size() - 1; i >= 0; --i) {
        text.remove(removals.at(i).first, removals.at(i).second);
    }
    return text;
}

namespace {  // Internal helpers, continued

// Cheap "how much real Vietnamese is here" score, shared by salvage candidate ranking.
int vietnameseContentScore(const QString &text)
{
    return vietnameseAccentCount(text) * 8 + latinWordCount(text) * 3;
}

// Function calc score for translate line
int calculateLineScore(const QString &line, SourceLanguage language)
{
    int score = latinWordCount(line) * 6;
    score += vietnameseAccentCount(line) * 8;  // Bonus with Vietnamese accent characters
    score -= hanCharCount(line) * 12;          // Penalize Han, but keep mixed candidates for retry analysis

    if (language == SourceLanguage::English) {
        // With a Latin-script source, raw Latin characters are not evidence of a
        // translation at all — scoring them would rank the untranslated source line
        // highest of every candidate. Penalise words that do not fit Vietnamese instead.
        score -= foreignLatinWordCount(line) * 10;
    } else {
        score += englishCharCount(line);
    }

    return score;
}

QString retryInstructionForIssue(TranslationIssue issue)
{
    switch (issue) {
        case TranslationIssue::NoUsableVietnameseCandidate:
            return QStringLiteral("The previous output was not a usable Vietnamese subtitle. Translate again into Vietnamese only.");
        case TranslationIssue::ContainsHan:
            return QStringLiteral("The previous output copied Chinese characters. Translate the entire line into Vietnamese and do not copy Chinese text.");
        case TranslationIssue::ResidualHan:
            return QStringLiteral("The previous output was mostly Vietnamese but still contained Chinese characters. Replace every remaining Chinese name, place, and term with Vietnamese Sino-Vietnamese words.");
        case TranslationIssue::ResidualEnglish:
            return QStringLiteral("The previous output was mostly Vietnamese but left a few English words untranslated. Translate every remaining English word into Vietnamese; keep only proper names.");
        case TranslationIssue::CopiedEnglishSource:
            return QStringLiteral("The previous output repeated the English source line instead of translating it. Write the meaning in Vietnamese; do not copy English words.");
        case TranslationIssue::EnglishHeavy:
            return QStringLiteral("The previous output was English-heavy. Use Vietnamese only.");
        case TranslationIssue::OverExpanded:
            return QStringLiteral("The previous output over-explained a short phrase. Keep it concise and subtitle-like.");
        case TranslationIssue::TooShort:
            return QStringLiteral("The previous output was too short or incomplete. Translate the full meaning.");
        case TranslationIssue::TooLong:
            return QStringLiteral("The previous output was too long. Make it shorter and subtitle-like.");
        case TranslationIssue::Repeated:
            return QStringLiteral("The previous output repeated text. Do not repeat phrases.");
        case TranslationIssue::UnexpectedEnglish:
            return QStringLiteral("The previous output contained unwanted English or meta words. Use Vietnamese only.");
        case TranslationIssue::Empty:
            return QStringLiteral("The previous output was empty or invalid. Translate again into Vietnamese only.");
        case TranslationIssue::LowScore:
            return QStringLiteral("The previous output did not look like natural Vietnamese. Translate again into one clean Vietnamese subtitle line.");
        case TranslationIssue::None:
            return {};
    }

    return {};
}

void replaceLatinAliasWholeWord(QString &text, const QString &alias, const QString &replacement)
{
    if (text.isEmpty() || alias.isEmpty()) {
        return;
    }

    QString loweredText = text.toLower();
    const QString loweredAlias = alias.toLower();
    int searchFrom = 0;

    while (searchFrom < loweredText.size()) {
        const int idx = loweredText.indexOf(loweredAlias, searchFrom);
        if (idx < 0) {
            break;
        }

        const int end = idx + loweredAlias.size();
        const bool leftOk = idx == 0 || isWordBoundaryChar(text.at(idx - 1));
        const bool rightOk = end >= text.size() || isWordBoundaryChar(text.at(end));

        if (leftOk && rightOk) {
            text.replace(idx, alias.size(), replacement);
            loweredText = text.toLower();
            searchFrom = idx + replacement.size();
        } else {
            searchFrom = idx + alias.size();
        }
    }
}

void appendAliasRule(QVector<QPair<QString, QString>> &aliasPairs, QSet<QString> &dedupe,
                     const QString &alias, const QString &target)
{
    const QString aliasTrimmed = alias.trimmed();
    const QString targetTrimmed = target.trimmed();
    if (aliasTrimmed.isEmpty() || targetTrimmed.isEmpty()) {
        return;
    }

    const QString key = aliasTrimmed + QStringLiteral("\u001f") + targetTrimmed;
    if (dedupe.contains(key)) {
        return;
    }

    aliasPairs.push_back({aliasTrimmed, targetTrimmed});
    dedupe.insert(key);
}

bool isShortSourcePhrase(const QString &sourceText, SourceLanguage language)
{
    const int units = sourceUnitCount(sourceText, language);
    return units > 0 && units <= tuning::profileFor(language).shortSourceUnitLimit;
}

// Set of lower-cased Latin word stems present in a line, used to measure how much of an
// English source line survived verbatim into the translation.
QSet<QString> latinWordSet(const QString &text)
{
    static const QRegularExpression kToken(QStringLiteral("[A-Za-zÀ-ỹĐđ]+"));
    QSet<QString> words;
    auto it = kToken.globalMatch(text);
    while (it.hasNext()) {
        words.insert(it.next().captured(0).toLower());
    }
    return words;
}

// The words of an English source line that a correct translation is expected to REPLACE.
// Capitalized tokens are dropped: proper names are supposed to survive into the Vietnamese
// output, so counting them as evidence of copying would flag a card like "Jack, Berlin" —
// where leaving both words alone is exactly right — as an untranslated echo.
//
// Subtitles are often typeset in full caps, which makes capitalization carry no information
// at all; in that case every word is kept rather than throwing the whole line away.
QSet<QString> translatableSourceWords(const QString &sourceText)
{
    static const QRegularExpression kToken(QStringLiteral("[A-Za-zÀ-ỹĐđ]+"));
    const bool capsAreUninformative = (sourceText == sourceText.toUpper());

    QSet<QString> words;
    auto it = kToken.globalMatch(sourceText);
    while (it.hasNext()) {
        const QString token = it.next().captured(0);
        if (!capsAreUninformative && token.at(0).isUpper()) {
            continue;
        }
        words.insert(token.toLower());
    }
    return words;
}

bool containsModelMetaText(const QString &text)
{
    static const QStringList keywords = {
        QStringLiteral("repair it"),
        QStringLiteral("rewrite"),
        QStringLiteral("rephrase"),
        QStringLiteral("translate"),
        QStringLiteral("translation"),
        QStringLiteral("natural translation"),
        QStringLiteral("make it natural"),
        QStringLiteral("to be natural"),
        QStringLiteral("improved version"),
        QStringLiteral("corrected version"),
        QStringLiteral("explanation"),
        QStringLiteral("note:"),
        QStringLiteral("output:"),
        QStringLiteral("answer:")
    };

    const QString lower = text.toLower();

    for (const QString &kw : keywords) {
        if (lower.contains(kw))
            return true;
    }

    return false;
}

QStringList splitCandidateLines(const QString &text)
{
    QString normalized = text;

    // Collapse hard line breaks.
    normalized.replace(QRegularExpression(QStringLiteral("[\\r\\n]+")), QStringLiteral("\n"));

    // Treat '.' as a sentence separator, EXCEPT between two digits: Vietnamese groups
    // thousands with dots (e.g. "46.000"), so splitting there would strip part of a number.
    normalized.replace(QRegularExpression(QStringLiteral("(?<![0-9])\\.+|\\.+(?![0-9])")),
                       QStringLiteral("\n"));

    // Avoid splitting every Vietnamese hyphenated word blindly.
    normalized.replace(QRegularExpression(QStringLiteral("\\s*[-–—]\\s*(?=[A-Za-z]{2,}\\b)")),
                       QStringLiteral("\n"));

    return normalized.split('\n', Qt::SkipEmptyParts);
}

QString normalizePunctuation(QString text)
{
    static const QRegularExpression kCjkPunctuation(
        QStringLiteral("[。、「」『』【】]"));

    text.remove(kCjkPunctuation);

    static const QRegularExpression kLeadingNoise(
        QStringLiteral(
            "^[\\s,，。.!！？?;；:：\\-–—>\\(\\)]+"));

    static const QRegularExpression kTrailingNoise(
        QStringLiteral(
            "[\\s,，。.!！？?;；:：\\-–—>\\(\\)]+$"));

    text.remove(kLeadingNoise);
    text.remove(kTrailingNoise);

    return text.trimmed();
}

QString normalizeWhitespace(QString text)
{
    static const QRegularExpression kMultiSpace(QStringLiteral("\\s{2,}"));

    text.replace(kMultiSpace, QStringLiteral(" "));

    return text.trimmed();
}

} // anonymous namespace ************************************************************************

int sourceUnitCount(const QString &sourceText, SourceLanguage language)
{
    return language == SourceLanguage::English ? latinWordCount(sourceText)
                                               : hanCharCount(sourceText);
}

bool isLikelySourceSubtitle(const QString &text, SourceLanguage language)
{
    return language == SourceLanguage::English ? isLikelyEnglishSubtitle(text)
                                               : isLikelyChineseSubtitle(text);
}

bool isLikelyChineseSubtitle(const QString &text)
{
    int cjkCount = 0;
    int latinLetterCount = 0;
    for (const QChar ch : text) {
        if (ch.isSpace()) {
            continue;
        }

        if (isHanChar(ch)) {
            ++cjkCount;
        } else if (ch.isLetter()) {
            // Only Latin/other letters signal English or garbled OCR. Digits are NOT
            // counted against "Chineseness": Chinese subtitles legitimately carry long
            // numbers (unit numbers like "363741师团", years, quantities). Counting them
            // used to reject such lines as non-Chinese and skip translation entirely.
            ++latinLetterCount;
        }
    }

    return cjkCount >= 2 && cjkCount * 2 >= std::max(1, latinLetterCount);
}

bool isLikelyEnglishSubtitle(const QString &text)
{
    // Mirror of isLikelyChineseSubtitle for the English pipeline: reject reads that are
    // not plausibly an English subtitle line before spending a backend round-trip on them.
    // Han characters are the tell-tale of a wrong-model or hallucinated read here.
    if (hanCharCount(text) > 0) {
        return false;
    }

    int latinLetters = 0;
    int otherLetters = 0;
    for (const QChar ch : text) {
        if (!ch.isLetter()) {
            continue;
        }
        if (isEnglishChar(ch)) {
            ++latinLetters;
        } else {
            ++otherLetters;
        }
    }

    // At least one real word and a couple of letters: single stray glyphs ("I", "a") are
    // indistinguishable from OCR noise on a frame with no subtitle.
    if (latinLetters < 2 || latinWordCount(text) < 1) {
        return false;
    }

    // Accented letters in an English source mean the recognition drifted into another
    // script (or the translation window is being captured back into the OCR zone).
    return latinLetters >= otherLetters * 2;
}

bool containsHanCharacters(const QString &text)
{
    for (const QChar ch : text) {
        if (isHanChar(ch)) {
            return true;
        }
    }
    return false;
}

bool isEnglishHeavyOutput(const QString &text)
{

    int latinLetters = 0;
    int totalLetters = 0;
    int vnHintLetters = 0;

    QString normalizedText = text.normalized(QString::NormalizationForm_C);
    for (const QChar ch : normalizedText) {
        if (!ch.isLetter()) {
            continue;
        }

        ++totalLetters;
        const ushort u = ch.unicode();
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')) {
            ++latinLetters;
        }
        // Vietnamese-specific letters and common Latin extended range.
        if (QStringLiteral("ăâđêôơưĂÂĐÊÔƠƯ").contains(ch) || (u >= 0x00C0 && u <= 0x024F) ||
            (u >= 0x1EA0 && u <= 0x1EF9)) {
            ++vnHintLetters;
        }
    }

    if (totalLetters < 6) {
        return false;
    }

    const double latinRatio = static_cast<double>(latinLetters) / static_cast<double>(totalLetters);
    return vnHintLetters == 0 && latinRatio > 0.78;
}

bool isSuspiciouslyShortTranslation(const QString &sourceText, const QString &translatedText,
                                    SourceLanguage language)
{
    const tuning::LanguageProfile &profile = tuning::profileFor(language);
    const int srcUnits = sourceUnitCount(sourceText, language);
    const int outWords = latinWordCount(translatedText);

    if (srcUnits < profile.minUnitsForRatioCheck) {
        // Short source: ratio is too noisy; use a hard floor instead.
        // Requiring >= 3 units avoids false-positives for 1–2-unit sources (e.g. names).
        return srcUnits >= 3 && outWords <= 1;
    }

    // Longer source: ratio check only — all degenerate cases (empty, single-word,
    // near-empty chars) are already covered when ratio < minTranslationWordRatio.
    const double ratio = static_cast<double>(outWords) / static_cast<double>(srcUnits);
    return ratio < profile.minTranslationWordRatio;
}

bool isOverExpandedTranslation(const QString &sourceText, const QString &translatedText,
                               SourceLanguage language)
{
    if (!isShortSourcePhrase(sourceText, language)) {
        return false;
    }

    const int outWords = latinWordCount(translatedText);
    if (outWords >= 8) {
        return true;
    }

    // Any explicit multi-sentence output for a short source phrase is suspicious.
    static const QRegularExpression kSentenceBreak(QStringLiteral("[\\.!?。]+\\s+"));
    if (translatedText.contains(kSentenceBreak)) {
        return true;
    }

    return false;
}

bool isSuspiciouslyLongTranslation(const QString &sourceText, const QString &translatedText,
                                   SourceLanguage language)
{
    const tuning::LanguageProfile &profile = tuning::profileFor(language);
    const int srcUnits = sourceUnitCount(sourceText, language);

    if (srcUnits <= 0) {
        return false;
    }

    const int outWords = latinWordCount(translatedText);

    // Subtitle short but output unusually long.
    if (srcUnits <= profile.longSourceUnitThreshold) {
        return outWords > srcUnits * profile.maxWordRatioShortSource;
    }

    return outWords > srcUnits * profile.maxWordRatioLongSource;
}

bool isClearlyOverGenerated(const QString &sourceText, const QString &translatedText,
                            SourceLanguage language)
{
    const int srcLen = sourceText.trimmed().size();
    const int outLen = translatedText.trimmed().size();

    // The factor is per-language because the comparison is raw characters: Vietnamese runs
    // several times longer than its Han source but only marginally longer than its English
    // one, so the Chinese factor would never trip on an English line.
    return outLen > srcLen * tuning::profileFor(language).maxOutputLengthFactor;
}

bool isEchoOfEnglishSource(const QString &sourceText, const QString &translatedText)
{
    // With a Chinese source, an untranslated echo is caught by the script check. With an
    // English one there is no script boundary, so measure overlap instead: if most of the
    // words the model was supposed to replace come back verbatim, nothing was translated.
    const QSet<QString> sourceWords = translatableSourceWords(sourceText);
    if (sourceWords.size() < 2) {
        // Too little signal — a one-word line, or a card that is nothing but proper names.
        // The length guards cover these.
        return false;
    }

    const QSet<QString> outputWords = latinWordSet(translatedText);
    if (outputWords.isEmpty()) {
        return false;
    }

    int shared = 0;
    for (const QString &word : sourceWords) {
        if (outputWords.contains(word)) {
            ++shared;
        }
    }

    const double overlap = static_cast<double>(shared) / static_cast<double>(sourceWords.size());
    // A genuine Vietnamese translation shares only proper names with its English source.
    return overlap >= 0.6;
}

bool containsUnexpectedEnglish(const QString &text)
{
    static const QStringList badWords = {
        QStringLiteral("anymore"),
        QStringLiteral("however"),
        QStringLiteral("therefore"),
        QStringLiteral("translation"),
        QStringLiteral("subtitle"),
        QStringLiteral("context"),
        QStringLiteral("output"),
        QStringLiteral("answer"),
        QStringLiteral("vietnamese")
    };

    const QString lower = text.toLower();

    for (const QString &word : badWords) {
        if (lower.contains(word)) {
            return true;
        }
    }

    return false;
}

bool containsRepeatedSubtitleFragments(const QString &text)
{
    QString normalized = text.toLower();

    normalized.remove(QRegularExpression(QStringLiteral("[\\s,，。.!！？?;；:\"'`()\\[\\]{}]+")));

    if (normalized.size() < 12) {
        return false;
    }

    for (int fragmentLen = 6; fragmentLen <= normalized.size() / 2; ++fragmentLen) {
        const QString fragment = normalized.left(fragmentLen);

        if (fragment.isEmpty()) {
            continue;
        }

        const int count = normalized.count(fragment);

        if (count >= 3) {
            return true;
        }
    }

    return false;
}

QString stripReasoningBlocks(QString text)
{
    // Balanced blocks, including empty "<think></think>" and multi-line reasoning.
    static const QRegularExpression kThinkBlock(
        QStringLiteral("<think>.*?</think>"),
        QRegularExpression::DotMatchesEverythingOption);
    text.remove(kThinkBlock);

    // An unterminated block (answer truncated inside the reasoning) — drop from the tag on.
    const int danglingOpen = text.indexOf(QStringLiteral("<think>"));
    if (danglingOpen >= 0) {
        text.truncate(danglingOpen);
    }

    // Any leftover stray tags.
    static const QRegularExpression kThinkTag(QStringLiteral("</?think>"));
    text.remove(kThinkTag);

    return text.trimmed();
}

QString selectBestVietnameseLine(const QString &text, SourceLanguage language)
{
    QStringList lines = splitCandidateLines(text);

    if (lines.isEmpty())
        return text;

    if (lines.size() > 3)
        lines = lines.mid(0, 3);

    QString bestLine;
    int bestScore = -1000000;

    for (QString line : lines) {
        line = line.trimmed();

        if (line.isEmpty())
            continue;

        // Skip lines that are likely to be model meta-text or instructions
        if (containsModelMetaText(line))
            continue;

        // Calc Score for line
        int score = calculateLineScore(line, language);

        if (score > bestScore) {
            bestScore = score;
            bestLine = line;
        }
    }

    // Check score of best line, if too low, return empty string
    if (bestScore < tuning::kTranslateLineScoreMin) {
        qDebug() << "selectBestVietnameseLine: best line score too low, bestScore=" << bestScore
                 << "line=" << bestLine;
        return QString();
    }

    return bestLine;
}

QString salvageVietnameseFragment(const QString &rawText)
{
    QStringList lines = splitCandidateLines(rawText);
    if (lines.isEmpty()) {
        lines = QStringList{rawText};
    }

    QString bestLine;
    int bestScore = -1;

    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || containsModelMetaText(line)) {
            continue;
        }

        // Strip everything that made the line fail the gate, keeping Vietnamese words.
        QString cleaned = removeHanCharacters(line);
        cleaned = removeForeignLatinWords(cleaned);
        cleaned = normalizeTranslation(cleaned);

        // Meta-text often leaves stray full-width / CJK punctuation and empty quote pairs
        // behind once the Han runs are removed (e.g. `... nhiều ，""`). Drop CJK symbols,
        // full-width forms, and quote characters before the final punctuation pass.
        static const QRegularExpression kStrayPunct(
            QStringLiteral("[\\x{3000}-\\x{303F}\\x{FF00}-\\x{FFEF}\"'`]+"));
        cleaned.remove(kStrayPunct);

        cleaned = normalizePunctuation(cleaned);
        cleaned = normalizeWhitespace(cleaned);
        if (cleaned.isEmpty()) {
            continue;
        }

        const int score = vietnameseContentScore(cleaned);
        if (score > bestScore) {
            bestScore = score;
            bestLine = cleaned;
        }
    }

    if (bestLine.isEmpty()) {
        return {};
    }

    // A single word recovered from a line that already failed the quality gate is almost
    // always noise (e.g. "Thử" salvaged from "企图趁我抗日军民)"), so require at least two
    // words; without a diacritic require more, since accent-free text is weaker evidence.
    const int words = latinWordCount(bestLine);
    if (words < 2) {
        return {};
    }
    if (!hasVietnameseDiacritic(bestLine) && words < 3) {
        return {};
    }

    return bestLine;
}

QString shortText(const QString &text, int maxChars)
{
    QString cleaned = text.trimmed();
    if (cleaned.size() <= maxChars) {
        return cleaned;
    }
    return cleaned.left(std::max(0, maxChars - 1)).trimmed() + QStringLiteral("…");
}

QString normalizeTranslation(QString text)
{
    text = text.trimmed();

    // Check empty string
    if (text.isEmpty()) {
        return text;
    }

    if (text.startsWith(QStringLiteral("```"))) {
        const QStringList parts = text.split(QStringLiteral("```"), Qt::SkipEmptyParts);
        if (!parts.isEmpty()) {
            text = parts.first().trimmed();
        }
    }

    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    if (!lines.isEmpty()) {
        text = lines.first().trimmed();
    }

    static const QStringList prefixes = {
        QStringLiteral("Vietnamese:"),
        QStringLiteral("Vietnamese subtitle:"),
        QStringLiteral("越南语"),
        QStringLiteral("译文"),
        QStringLiteral("Output"),
        QStringLiteral("Output:"),
        QStringLiteral("Answer:"),
        QStringLiteral("Bản dịch:"),
        QStringLiteral("Dịch:"),
        QStringLiteral("Translation:")};
    for (const QString &prefix : prefixes) {
        if (text.startsWith(prefix, Qt::CaseInsensitive)) {
            text = text.mid(prefix.size()).trimmed();
            break;
        }
    }

    if ((text.startsWith('"') && text.endsWith('"')) ||
        (text.startsWith('\'') && text.endsWith('\''))) {
        text = text.mid(1, text.size() - 2).trimmed();
    }

    return text;
}

QString sanitizeFinalTranslation(QString text)
{
    text = normalizeTranslation(text);

    // Some local models emit intermediate candidates joined by arrows.
    // Keep the last candidate because it is usually the final refinement.
    static const QRegularExpression kArrowSeparator(QStringLiteral("\\s*(?:->|=>|→)\\s*"));
    if (text.contains(kArrowSeparator)) {
        const QStringList candidates = text.split(kArrowSeparator, Qt::SkipEmptyParts);
        if (!candidates.isEmpty()) {
            text = candidates.constLast().trimmed();
        }
    }

    const int asciiColon = text.lastIndexOf(':');
    const int fullWidthColon = text.lastIndexOf(QChar(0xFF1A));
    const int splitAt = std::max(asciiColon, fullWidthColon);
    if (splitAt >= 0 && splitAt + 1 < text.size()) {
        const QString tail = text.mid(splitAt + 1).trimmed();
        if (!tail.isEmpty()) {
            text = tail;
        }
    }

    static const QRegularExpression kMultiSpace(QStringLiteral("\\s{2,}"));
    text.replace(kMultiSpace, QStringLiteral(" "));

    static const QRegularExpression kDotSpam(QStringLiteral("(?:。\\s*){4,}|(?:\\.\\s*){6,}"));
    text.remove(kDotSpam);

    // Remove stray CJK punctuation artifacts that can leak from source text.
    static const QRegularExpression kCjkPunctuation(QStringLiteral("[。、「」『』【】]"));
    text.remove(kCjkPunctuation);

    text = text.trimmed();
    while (!text.isEmpty() && (text.front() == ':' || text.front() == '-' || text.front() == '"')) {
        text.remove(0, 1);
        text = text.trimmed();
    }

    return text;
}

QString removeHanCharacters(QString text)
{
    static const QRegularExpression kHanRegex(
        QStringLiteral("[\\x{3400}-\\x{4DBF}\\x{4E00}-\\x{9FFF}\\x{F900}-\\x{FAFF}]+"));

    text.remove(kHanRegex);
    text = text.simplified();

    return text.trimmed();
}

QString postProcessTranslation(QString text)
{
    text = text.trimmed();
    if (text.isEmpty()) {
        return text;
    }

    text = normalizePunctuation(text);
    text = normalizeWhitespace(text);

    qDebug() << "[POST-PROCESS] Translation:" << text;

    return text.trimmed();
}

AliasData loadAliasRules(const QString &path)
{
    AliasData result;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return result;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "TranslationTextProcessor: failed to parse alias JSON:" << err.errorString();
        return result;
    }

    const QJsonObject root = doc.object();
    const QJsonObject glossary = root.value(QStringLiteral("glossary")).toObject();
    const QJsonObject aliases = root.value(QStringLiteral("aliases")).toObject();

    QSet<QString> glossaryDedupe;
    for (auto it = glossary.constBegin(); it != glossary.constEnd(); ++it) {
        const QString sourceTerm = it.key().trimmed();
        const QString target = it.value().toString().trimmed();
        if (sourceTerm.isEmpty() || target.isEmpty()) {
            continue;
        }

        appendAliasRule(result.glossaryPairs, glossaryDedupe, sourceTerm, target);
    }

    QSet<QString> aliasDedupe;
    for (auto it = aliases.constBegin(); it != aliases.constEnd(); ++it) {
        const QString alias = it.key().trimmed();
        const QString target = it.value().toString().trimmed();
        if (alias.isEmpty() || target.isEmpty()) {
            continue;
        }

        appendAliasRule(result.aliasPairs, aliasDedupe, alias, target);
    }

    // Longer terms first to avoid partial replacements before full names.
    std::sort(result.glossaryPairs.begin(), result.glossaryPairs.end(),
              [](const QPair<QString, QString> &left, const QPair<QString, QString> &right) {
                  return left.first.size() > right.first.size();
              });

    std::sort(result.aliasPairs.begin(), result.aliasPairs.end(),
              [](const QPair<QString, QString> &left, const QPair<QString, QString> &right) {
                  return left.first.size() > right.first.size();
              });

    return result;
}

QString applyAliasNormalization(const QString &translatedText,
                                   const QVector<QPair<QString, QString>> &aliasPairs)
{
    if (translatedText.isEmpty() || aliasPairs.isEmpty()) {
        return translatedText;
    }

    QString out = translatedText;
    for (const auto &pair : aliasPairs) {
        const QString &alias = pair.first;
        const QString &target = pair.second;
        if (alias.isEmpty() || target.isEmpty()) {
            continue;
        }

        if (hasLatinLetter(alias)) {
            replaceLatinAliasWholeWord(out, alias, target);
        } else {
            out.replace(alias, target);
        }
    }

    out = out.simplified();
    return out;
}

QString buildGlossaryBlockForSource(const QString &sourceText,
                                    const QVector<QPair<QString, QString>> &glossaryPairs)
{
    if (sourceText.isEmpty() || glossaryPairs.isEmpty()) {
        return {};
    }

    constexpr int kMaxGlossaryLines = 8;
    constexpr int kMaxGlossaryChars = 360;

    const QString normalizedSource = sourceText.normalized(QString::NormalizationForm_C);

    QStringList lines;
    QSet<QString> dedupe;

    for (const auto &pair : glossaryPairs) {
        const QString &sourceTerm = pair.first;
        const QString &target = pair.second;
        if (sourceTerm.isEmpty() || target.isEmpty()) {
            continue;
        }

        bool matched = false;
        if (hasLatinLetter(sourceTerm)) {
            matched = containsLatinWholeWordCaseInsensitive(normalizedSource, sourceTerm);
        } else {
            matched = normalizedSource.contains(sourceTerm);
        }

        if (!matched) {
            continue;
        }

        const QString key = sourceTerm + QStringLiteral("\u001f") + target;
        if (dedupe.contains(key)) {
            continue;
        }

        lines.append(sourceTerm + QStringLiteral(" = ") + target);
        dedupe.insert(key);

        if (lines.size() >= kMaxGlossaryLines || lines.join(QStringLiteral("\n")).size() >= kMaxGlossaryChars) {
            break;
        }
    }

    if (lines.isEmpty()) {
        return {};
    }

    QString block = lines.join(QStringLiteral("\n"));
    if (block.size() > kMaxGlossaryChars) {
        block = block.left(kMaxGlossaryChars).trimmed();
    }

    return block;
}

namespace {

// Rule block for the first-pass prompt. The two languages need opposite instructions on
// proper names — Chinese names are converted to their Sino-Vietnamese readings, whereas
// Western names must be left in Latin script untouched — so neither prompt is a
// parameterisation of the other.
QString firstPassRules(SourceLanguage language)
{
    if (language == SourceLanguage::English) {
        return QStringLiteral(
            "You are translating OCR subtitles for an English-language film into natural Vietnamese subtitle style.\n"
            "Rules:\n"
            "- Output ONLY the Vietnamese subtitle.\n"
            "- Absolutely NO explanations, NO notes, NO labels, No extra text, One line only.\n"
            "- Translate every English word. Do not leave any English in the output.\n"
            "- Keep Western proper names (people, places, brands) in their original Latin spelling.\n"
            "- Do NOT repeat or quote the English source line.\n"
            "- Use natural spoken Vietnamese with the right pronoun for the speakers, not word-for-word English word order.\n"
            "- Keep the translation concise, natural, and suitable for on-screen subtitles.\n"
            "- Never output words like 'Vietnamese', 'Output', 'Rules', or any instruction text.\n");
    }

    return QStringLiteral(
        "You are translating OCR subtitles for a Chinese film into natural Vietnamese subtitle style.\n"
        "Rules:\n"
        "- Output ONLY the Vietnamese subtitle.\n"
        "- Absolutely NO explanations, NO notes, NO labels, No extra text, One line only.\n"
        "- No Chinese characters.\n"
        "- Convert Chinese names into Vietnamese Sino-Vietnamese readings.\n"
        "- Keep the translation concise, natural, and suitable for on-screen subtitles.\n"
        "- Never output words like 'Vietnamese', 'Output', 'Rules', or any instruction text.\n");
}

QString retryRules(SourceLanguage language)
{
    if (language == SourceLanguage::English) {
        return QStringLiteral(
            "Rules:\n"
            "- Rewrite entirely in Vietnamese.\n"
            "- No English words except unavoidable proper names.\n"
            "- Do NOT copy the English source line.\n"
            "- Output only one Vietnamese subtitle line.\n"
            "- No explanations, no notes, no labels, no extra text.\n"
            "- Keep the translation concise, natural, and suitable for on-screen subtitles.\n");
    }

    return QStringLiteral(
        "Rules:\n"
        "- Rewrite entirely in Vietnamese.\n"
        "- No Chinese Han characters.\n"
        "- No English words unless they are unavoidable proper names.\n"
        "- Convert all Chinese names to Sino-Vietnamese.\n"
        "- Output only one Vietnamese subtitle line.\n"
        "- No explanations, no notes, no labels, no extra text.\n"
        "- Keep the translation concise, natural, and suitable for on-screen subtitles.\n");
}

} // namespace

// Translation prompt for LLM model, with context and recent dialogue history.
QString translationPrompt(const QString &sourceText,
                         const QString &recentDialogueContext,
                         const QString &glossaryBlock,
                         SourceLanguage language)
{
    QString prompt = firstPassRules(language);

    if (!glossaryBlock.isEmpty()) {
        prompt += QStringLiteral(
            "\nGlossary for this line. Use these exact Vietnamese terms when they appear in the source:\n") +
            glossaryBlock + QStringLiteral("\n");
    }

    if (!recentDialogueContext.isEmpty()) {
        prompt += QStringLiteral("\nContext only. Do NOT translate these lines.\n\nPrevious:\n") +
                  recentDialogueContext + QStringLiteral("\n");
    }

    prompt += QStringLiteral("\nTranslate ONLY this line:\n") +
              sourceText + QStringLiteral("\nVietnamese subtitle:\n");

    // Print to debug prompt
    static bool debugPromptEnabled = true;
    if (debugPromptEnabled) {
        qDebug() << "Translation prompt:" << prompt;
        debugPromptEnabled = false; // Only print once per run to avoid clutter.
    }

    return prompt;
}

// Retry prompt for LLM model, with context and recent dialogue history.
QString translationRetryPrompt(const QString &sourceText,
                              const QString &recentDialogueContext,
                              const QString &previousTranslation,
                              const QString &glossaryBlock,
                              TranslationIssue issue,
                              SourceLanguage language)
{
    QString prompt = QStringLiteral("The previous translation is invalid.\n");

    const QString issueHint = retryInstructionForIssue(issue);
    if (!issueHint.isEmpty()) {
        prompt += QStringLiteral("Problem: ") + issueHint + QStringLiteral("\n");
    }

    prompt += retryRules(language);

    if (!glossaryBlock.isEmpty()) {
        prompt += QStringLiteral(
            "\nGlossary for this line. Use these exact Vietnamese terms when they appear in the source:\n") +
            glossaryBlock + QStringLiteral("\n");
    }

    if (!recentDialogueContext.isEmpty()) {
        prompt += QStringLiteral("\nContext only. Do NOT translate these lines.\n\nPrevious:\n") +
                  recentDialogueContext + QStringLiteral("\n");
    }

    prompt += QStringLiteral("\nTranslate ONLY this line:\n") +
              sourceText + QStringLiteral("\nPrevious translation:\n") +
              previousTranslation + QStringLiteral("\nVietnamese subtitle:\n");

    // print to debug prompt
    qDebug() << "Retry prompt:" << prompt;

    return prompt;
}

TranslationIssue evaluateTranslationQuality(const QString &sourceText,
                                            const QString &translatedText,
                                            SourceLanguage language)
{
    if (translatedText.trimmed().isEmpty()) {
        return TranslationIssue::Empty;
    }

    // Han in the output is always wrong, whichever language the source was: for a Chinese
    // source it means the line was copied instead of translated, for an English one it
    // means the model drifted into the wrong script entirely.
    const int outHan = hanCharCount(translatedText);
    if (outHan > 0) {
        const int outWords = latinWordCount(translatedText);
        if (outHan <= 3 && outWords >= 4) {
            return TranslationIssue::ResidualHan;
        }

        return TranslationIssue::ContainsHan;
    }

    if (isEnglishHeavyOutput(translatedText)) {
        return TranslationIssue::EnglishHeavy;
    }

    // The English pipeline's equivalent of the "copied the source" check. It has to run
    // before the length guards, which would otherwise report a verbatim echo as a
    // perfectly-proportioned translation.
    if (language == SourceLanguage::English && isEchoOfEnglishSource(sourceText, translatedText)) {
        return TranslationIssue::CopiedEnglishSource;
    }

    if (isOverExpandedTranslation(sourceText, translatedText, language)) {
        return TranslationIssue::OverExpanded;
    }

    if (isSuspiciouslyShortTranslation(sourceText, translatedText, language)) {
        return TranslationIssue::TooShort;
    }

    if (isSuspiciouslyLongTranslation(sourceText, translatedText, language) ||
        isClearlyOverGenerated(sourceText, translatedText, language)) {
        return TranslationIssue::TooLong;
    }

    if (containsRepeatedSubtitleFragments(translatedText)) {
        return TranslationIssue::Repeated;
    }

    if (containsUnexpectedEnglish(translatedText)) {
        return TranslationIssue::UnexpectedEnglish;
    }

    // A handful of untranslated English words in an otherwise Vietnamese line is the
    // English pipeline's residual case: cheap to repair by dropping them, so it gets its
    // own issue rather than being lumped in with a wholesale failure.
    const int foreignWords = foreignLatinWordCount(translatedText);
    if (foreignWords > 0) {
        if (language == SourceLanguage::English &&
            latinWordCount(translatedText) >= foreignWords * 3) {
            return TranslationIssue::ResidualEnglish;
        }
        return TranslationIssue::UnexpectedEnglish;
    }

    return TranslationIssue::None;
}

QString translationIssueMessage(TranslationIssue issue)
{
    switch (issue) {
        case TranslationIssue::None:
            return QStringLiteral("Translation quality check passed");
        case TranslationIssue::Empty:
            return QStringLiteral("Translation backend output is empty");
        case TranslationIssue::NoUsableVietnameseCandidate:
            return QStringLiteral("Translation backend output rejected: no usable Vietnamese candidate");
        case TranslationIssue::ContainsHan:
            return QStringLiteral("Translation backend output contains too many Han characters");
        case TranslationIssue::ResidualHan:
            return QStringLiteral("Translation backend output contains residual Han characters");
        case TranslationIssue::ResidualEnglish:
            return QStringLiteral("Translation backend output contains residual untranslated English words");
        case TranslationIssue::CopiedEnglishSource:
            return QStringLiteral("Translation backend output copied the English source line instead of translating it");
        case TranslationIssue::EnglishHeavy:
            return QStringLiteral("Translation backend output rejected: English-heavy output");
        case TranslationIssue::OverExpanded:
            return QStringLiteral("Translation backend output rejected: over-expanded short source translation");
        case TranslationIssue::TooShort:
            return QStringLiteral("Translation backend output rejected: suspiciously short translation");
        case TranslationIssue::TooLong:
            return QStringLiteral("Translation backend output rejected: suspiciously long translation");
        case TranslationIssue::Repeated:
            return QStringLiteral("Translation backend output rejected: repeated subtitle fragments");
        case TranslationIssue::UnexpectedEnglish:
            return QStringLiteral("Translation backend output rejected: unexpected English tokens");
        case TranslationIssue::LowScore:
            return QStringLiteral("Translation backend output rejected: low Vietnamese candidate score");
    }

    return QStringLiteral("Translation backend output rejected: unknown quality issue");
}

} // namespace TranslationTextProcessor
