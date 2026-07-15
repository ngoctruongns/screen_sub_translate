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

QString hanVariantFallback(QString text)
{
    if (text.isEmpty()) {
        return text;
    }

    // Minimal Traditional -> Simplified fallback for frequent OCR glossary mismatches.
    text.replace(QChar(0x58DE), QChar(0x574F)); // 壞 -> 坏
    text.replace(QChar(0x9435), QChar(0x94C1)); // 鐵 -> 铁
    text.replace(QChar(0x8ECC), QChar(0x8F68)); // 軌 -> 轨

    return text;
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

// Function calc score for translate line
int calculateLineScore(const QString &line)
{
    int score = englishCharCount(line);
    score += latinWordCount(line) * 6;
    score += vietnameseAccentCount(line) * 8;  // Bonus with Vietnamese accent characters
    score -= hanCharCount(line) * 12;          // Penalize Han, but keep mixed candidates for retry analysis

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

bool isShortSourcePhrase(const QString &sourceText)
{
    const int srcHan = hanCharCount(sourceText);
    return srcHan > 0 && srcHan <= 5;
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

    // Split normal line/sentence separators.
    normalized.replace(QRegularExpression(QStringLiteral("[\\r\\n.]+")), QStringLiteral("\n"));

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

bool isLikelyChineseSubtitle(const QString &text)
{
    int cjkCount = 0;
    int letterOrDigitCount = 0;
    for (const QChar ch : text) {
        if (ch.isSpace()) {
            continue;
        }

        if (isHanChar(ch)) {
            ++cjkCount;
        }
        if (ch.isLetterOrNumber()) {
            ++letterOrDigitCount;
        }
    }

    return cjkCount >= 2 && cjkCount * 2 >= std::max(1, letterOrDigitCount);
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

bool isSuspiciouslyShortTranslation(const QString &sourceText, const QString &translatedText)
{
    const int srcHan = hanCharCount(sourceText);
    const int outWords = latinWordCount(translatedText);

    if (srcHan < tuning::kMinHanCharsForRatioCheck) {
        // Short source: ratio is too noisy; use a hard floor instead.
        // srcHan >= 3 avoids false-positives for 1–2-char sources (e.g. names).
        return srcHan >= 3 && outWords <= 1;
    }

    // Longer source: ratio check only — all degenerate cases (empty, single-word,
    // near-empty chars) are already covered when ratio < kMinTranslationWordRatio.
    const double ratio = static_cast<double>(outWords) / static_cast<double>(srcHan);
    return ratio < tuning::kMinTranslationWordRatio;
}

bool isOverExpandedTranslation(const QString &sourceText, const QString &translatedText)
{
    if (!isShortSourcePhrase(sourceText)) {
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

bool isSuspiciouslyLongTranslation(const QString &sourceText, const QString &translatedText)
{
    const int srcHan = hanCharCount(sourceText);

    if (srcHan <= 0) {
        return false;
    }

    const int outWords = latinWordCount(translatedText);

    // Subtitle short but output unusually long.
    if (srcHan <= 8) {
        return outWords > srcHan * 4;
    }

    return outWords > srcHan * 3;
}

bool isClearlyOverGenerated(const QString &sourceText, const QString &translatedText)
{
    const int srcLen = sourceText.trimmed().size();
    const int outLen = translatedText.trimmed().size();

    return outLen > srcLen * 12;
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

QString selectBestVietnameseLine(const QString &text)
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
        int score = calculateLineScore(line);

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

QString loadPromptContext(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QString text = QString::fromUtf8(file.readAll()).trimmed();
    if (text.size() > tuning::kTranslatePromptContextMaxChars) {
        text = text.left(tuning::kTranslatePromptContextMaxChars).trimmed();
    }
    return text;
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
            if (!matched) {
                matched = normalizedSource.contains(hanVariantFallback(sourceTerm));
            }
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

// Translation prompt for LLM model, with context and recent dialogue history.
QString translationPrompt(const QString &sourceText,
                         const QString &contextBlock,
                         const QString &recentDialogueContext,
                         const QString &glossaryBlock)
{
    QString prompt = QStringLiteral(
        "You are translating OCR subtitles for a Chinese film into natural Vietnamese subtitle style.\n"
        "Rules:\n"
        "- Output ONLY the Vietnamese subtitle.\n"
        "- Absolutely NO explanations, NO notes, NO labels, No extra text, One line only.\n"
        "- No Chinese characters.\n"
        "- Convert Chinese names into Vietnamese Sino-Vietnamese readings.\n"
        "- Keep the translation concise, natural, and suitable for on-screen subtitles.\n"
        "- Never output words like 'Vietnamese', 'Output', 'Rules', or any instruction text.\n"
    );

    if (!contextBlock.isEmpty()) {
        prompt += QStringLiteral("\nMovie context provided by user:\n") + contextBlock + QStringLiteral("\n");
    }

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
        // debugPromptEnabled = false; // Only print once per run to avoid clutter.
    }

    return prompt;
}

// Retry prompt for LLM model, with context and recent dialogue history.
QString translationRetryPrompt(const QString &sourceText,
                              const QString &recentDialogueContext,
                              const QString &previousTranslation,
                              const QString &glossaryBlock,
                              TranslationIssue issue)
{
    QString prompt = QStringLiteral("The previous translation is invalid.\n");

    const QString issueHint = retryInstructionForIssue(issue);
    if (!issueHint.isEmpty()) {
        prompt += QStringLiteral("Problem: ") + issueHint + QStringLiteral("\n");
    }

    prompt += QStringLiteral(
        "Rules:\n"
        "- Rewrite entirely in Vietnamese.\n"
        "- No Chinese Han characters.\n"
        "- No English words unless they are unavoidable proper names.\n"
        "- Convert all Chinese names to Sino-Vietnamese.\n"
        "- Output only one Vietnamese subtitle line.\n"
        "- No explanations, no notes, no labels, no extra text.\n"
        "- Keep the translation concise, natural, and suitable for on-screen subtitles.\n"
    );

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

TranslationIssue evaluateTranslationQuality(const QString &sourceText, const QString &translatedText)
{
    if (translatedText.trimmed().isEmpty()) {
        return TranslationIssue::Empty;
    }

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

    if (isOverExpandedTranslation(sourceText, translatedText)) {
        return TranslationIssue::OverExpanded;
    }

    if (isSuspiciouslyShortTranslation(sourceText, translatedText)) {
        return TranslationIssue::TooShort;
    }

    if (isSuspiciouslyLongTranslation(sourceText, translatedText) ||
        isClearlyOverGenerated(sourceText, translatedText)) {
        return TranslationIssue::TooLong;
    }

    if (containsRepeatedSubtitleFragments(translatedText)) {
        return TranslationIssue::Repeated;
    }

    if (containsUnexpectedEnglish(translatedText)) {
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
