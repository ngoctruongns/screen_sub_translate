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

namespace
{

bool hasLatinLetter(const QString &text)
{
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')) {
            return true;
        }
    }
    return false;
}

bool isWordBoundaryChar(const QChar ch)
{
    return !ch.isLetterOrNumber() && ch != QChar('_');
}

void replaceLatinAliasWholeWord(QString &text,
                                const QString &alias,
                                const QString &replacement)
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

void appendAliasRule(QVector<QPair<QString, QString>> &aliasPairs,
                     QSet<QString> &dedupe,
                     const QString &alias,
                     const QString &target)
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

} // anonymous namespace

bool isLikelyChineseSubtitle(const QString &text)
{
    int cjkCount = 0;
    int letterOrDigitCount = 0;
    for (const QChar ch : text) {
        if (ch.isSpace()) {
            continue;
        }

        const ushort u = ch.unicode();
        const bool isCjk =
            (u >= 0x3400 && u <= 0x4DBF) ||
            (u >= 0x4E00 && u <= 0x9FFF) ||
            (u >= 0xF900 && u <= 0xFAFF);
        if (isCjk) {
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
        const ushort u = ch.unicode();
        const bool isHan =
            (u >= 0x3400 && u <= 0x4DBF) ||
            (u >= 0x4E00 && u <= 0x9FFF) ||
            (u >= 0xF900 && u <= 0xFAFF);
        if (isHan) {
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

    for (const QChar ch : text) {
        if (!ch.isLetter()) {
            continue;
        }

        ++totalLetters;
        const ushort u = ch.unicode();
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')) {
            ++latinLetters;
        }
        // Vietnamese-specific letters and common Latin extended range.
        if (QStringLiteral("ăâđêôơưĂÂĐÊÔƠƯ").contains(ch) || (u >= 0x00C0 && u <= 0x024F)) {
            ++vnHintLetters;
        }
    }

    if (totalLetters < 6) {
        return false;
    }

    const double latinRatio = static_cast<double>(latinLetters) / static_cast<double>(totalLetters);
    return vnHintLetters == 0 && latinRatio > 0.78;
}

int hanCharCount(const QString &text)
{
    int count = 0;
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        const bool isHan =
            (u >= 0x3400 && u <= 0x4DBF) ||
            (u >= 0x4E00 && u <= 0x9FFF) ||
            (u >= 0xF900 && u <= 0xFAFF);
        if (isHan) {
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

    static const QRegularExpression kHanRegex(
        QStringLiteral("[\\x{3400}-\\x{4DBF}\\x{4E00}-\\x{9FFF}\\x{F900}-\\x{FAFF}]+"));
    text.remove(kHanRegex);

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

QString postProcessTranslation(QString text)
{
    text = text.trimmed();
    if (text.isEmpty()) {
        return text;
    }

    // Remove common meta/instruction tails generated by local LLMs.
    const QStringList cutMarkers = {
        QStringLiteral("To keep the"),
        QStringLiteral("To correct the"),
        QStringLiteral("To correct this"),
        QStringLiteral("To translate the"),
        QStringLiteral("hard constraints"),
        QStringLiteral("rules:"),
        QStringLiteral("according to the rules"),
        QStringLiteral("here is the vietnamese subtitle"),
        QStringLiteral("here's the translation"),
        QStringLiteral("context provided"),
        QStringLiteral("hãy tiếp tục dịch"),
        QStringLiteral("hãy kiên nhẫn"),
        QStringLiteral("tập trung vào nội dung"),
        QStringLiteral("phụ đề ngắn gọn"),
        QStringLiteral("phong cách tự nhiên")
    };

    int cutAt = -1;
    for (const QString &marker : cutMarkers) {
        const int idx = text.indexOf(marker, 0, Qt::CaseInsensitive);
        if (idx >= 0 && (cutAt < 0 || idx < cutAt)) {
            cutAt = idx;
        }
    }
    if (cutAt > 0) {
        text = text.left(cutAt).trimmed();
    }

    // Remove empty/garbage bracket fragments like (，), (...), etc.
    static const QRegularExpression kGarbageBrackets(
        QStringLiteral("[\\(（]\\s*[,，。…\\.\\-\\s]+[\\)）]"));
    text.remove(kGarbageBrackets);

    // Remove common noisy English tokens seen in bad generations.
    static const QRegularExpression kNoisyTokens(
        QStringLiteral("\\b(?:LinkedIn|Bold|scatter|subtitle|translation|context|rules?|Vietnamese|output|answer)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    text.remove(kNoisyTokens);

    // Remove empty-parenthesis artifacts frequently emitted by local models,
    // e.g. ") )", "() )", "( )".
    static const QRegularExpression kParenArtifacts(
        QStringLiteral("(?:[\\(（]\\s*[\\)）]|[\\)）]\\s*[\\)）])+")
    );
    text.remove(kParenArtifacts);

    // Remove known Vietnamese meta artifact injected by bad generations.
    static const QRegularExpression kNoisyVnMeta(
        QStringLiteral("Toàn\\s+bộ\\s+câu\\s+đã\\s+được\\s+OCR\\s+chính\\s+xác\\.?"),
        QRegularExpression::CaseInsensitiveOption);
    text.remove(kNoisyVnMeta);

    // Remove long dot/ellipsis spam blocks.
    static const QRegularExpression kDotSpam(QStringLiteral("(?:\\s*[。\\.…]\\s*){3,}"));
    text.replace(kDotSpam, QStringLiteral(" "));

    // Remove trailing English-only clause after punctuation.
    static const QRegularExpression kTrailingEnglishClause(
        QStringLiteral("[\\.;:!?]\\s*(?:[A-Za-z]{2,}\\s+){2,}[A-Za-z]{2,}\\s*$"));
    text.remove(kTrailingEnglishClause);

    // Remove short glued English suffix such as ".scatter".
    static const QRegularExpression kGluedEnglishSuffix(QStringLiteral("\\.\\s*[A-Za-z]{3,}\\b"));
    text.remove(kGluedEnglishSuffix);

    // Drop consecutive duplicated sentences to avoid repeated subtitle spam.
    const QStringList sentenceParts = text.split(QRegularExpression(QStringLiteral("(?<=[\\.!?。])\\s+")),
                                                 Qt::SkipEmptyParts);
    if (!sentenceParts.isEmpty()) {
        QStringList compact;
        compact.reserve(sentenceParts.size());
        QSet<QString> seen;
        for (const QString &part : sentenceParts) {
            const QString normalized = part.trimmed();
            if (normalized.isEmpty()) {
                continue;
            }
            if (seen.contains(normalized)) {
                continue;
            }
            compact.push_back(normalized);
            seen.insert(normalized);
        }
        text = compact.join(QStringLiteral(" "));
    }

    // Normalize repeated CJK periods and mixed period/space artifacts such as "。 。".
    static const QRegularExpression kRepeatedCjkPeriod(QStringLiteral("(?:。\\s*){2,}"));
    text.replace(kRepeatedCjkPeriod, QStringLiteral("。 "));

    static const QRegularExpression kMixedPeriodNoise(QStringLiteral("(?:\\.\\s*。|。\\s*\\.)+"));
    text.replace(kMixedPeriodNoise, QStringLiteral("。 "));

    static const QRegularExpression kMultiSpace(QStringLiteral("\\s{2,}"));
    text.replace(kMultiSpace, QStringLiteral(" "));

    // If model still returns chained alternatives, keep the final one.
    static const QRegularExpression kArrowSeparator(QStringLiteral("\\s*(?:->|=>|→)\\s*"));
    if (text.contains(kArrowSeparator)) {
        const QStringList candidates = text.split(kArrowSeparator, Qt::SkipEmptyParts);
        if (!candidates.isEmpty()) {
            text = candidates.constLast().trimmed();
        }
    }

    static const QRegularExpression kResidualCjkPunctuation(QStringLiteral("[。、「」『』【】]"));
    text.remove(kResidualCjkPunctuation);

    // Remove pure trailing punctuation/bracket debris left after cleanup passes.
    static const QRegularExpression kTrailingBracketNoise(
        QStringLiteral("(?:[\\s\\)）\\]】\\(（\\[【\\.。,，:：;；\\-–—])+$$"));
    text.remove(kTrailingBracketNoise);

    // Trim leading punctuation-only artifacts left by malformed generations
    // (e.g. "。 -> ", ":", "->").
    static const QRegularExpression kLeadingNoise(
        QStringLiteral("^(?:(?:->|=>|→)|[\\s。\\.…:：;；,，、\\-–—>])+")
    );
    text.remove(kLeadingNoise);

    // Trim trailing punctuation-only fragments left by cleanup passes
    // (e.g. "。 ：", "... ;", full-width variants).
    static const QRegularExpression kTrailingPunctNoise(
        QStringLiteral("[\\s。\\.…:：;；,，]+$"));
    text.remove(kTrailingPunctNoise);

    qDebug() << "[POST-PROCESS] Translation:" << text;

    return text.trimmed();
}

GlossaryData loadGlossary(const QString &path)
{
    GlossaryData result;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return result;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "TranslationTextProcessor: failed to parse glossary JSON:" << err.errorString();
        return result;
    }

    const QJsonObject root = doc.object();
    const QJsonObject glossary = root.value(QStringLiteral("glossary")).toObject();
    const QJsonObject aliases = root.value(QStringLiteral("aliases")).toObject();

    QStringList lines;
    QSet<QString> dedupe;

    for (auto it = glossary.constBegin(); it != glossary.constEnd(); ++it) {
        const QString key = it.key().trimmed();
        if (key.isEmpty()) {
            continue;
        }

        if (it.value().isString()) {
            const QString target = it.value().toString().trimmed();
            if (target.isEmpty()) {
                continue;
            }
            lines.append(key + QStringLiteral(" -> ") + target);
            appendAliasRule(result.aliasPairs, dedupe, key, target);
            continue;
        }

        if (it.value().isObject()) {
            const QJsonObject item = it.value().toObject();
            QString target = item.value(QStringLiteral("target")).toString().trimmed();
            if (target.isEmpty()) {
                target = item.value(QStringLiteral("value")).toString().trimmed();
            }
            if (target.isEmpty()) {
                continue;
            }

            lines.append(key + QStringLiteral(" -> ") + target);
            appendAliasRule(result.aliasPairs, dedupe, key, target);

            const QJsonArray aliasArray = item.value(QStringLiteral("aliases")).toArray();
            for (const QJsonValue &aliasVal : aliasArray) {
                if (!aliasVal.isString()) {
                    continue;
                }
                appendAliasRule(result.aliasPairs, dedupe, aliasVal.toString(), target);
            }
        }
    }

    for (auto it = aliases.constBegin(); it != aliases.constEnd(); ++it) {
        const QString alias = it.key().trimmed();
        const QString target = it.value().toString().trimmed();
        if (alias.isEmpty() || target.isEmpty()) {
            continue;
        }
        appendAliasRule(result.aliasPairs, dedupe, alias, target);
    }

    // Longer aliases first to avoid partial replacements before full names.
    std::sort(result.aliasPairs.begin(), result.aliasPairs.end(),
              [](const QPair<QString, QString> &left, const QPair<QString, QString> &right) {
                  return left.first.size() > right.first.size();
              });

    if (!lines.isEmpty()) {
        lines.removeDuplicates();
        result.promptLines = lines.join('\n');
    }

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

QString applyGlossaryNormalization(const QString &translatedText,
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

QString translationPrompt(const QString &sourceText,
                         const QString &contextBlock,
                         const QString &recentDialogueContext)
{
    QString prompt = QStringLiteral(
        "You are translating OCR subtitles for a Chinese historical war film into natural Vietnamese subtitle style.\n"
        "Rules:\n"
        "- Output ONLY the Vietnamese subtitle.\n"
        "- Absolutely NO explanations, NO notes, NO labels, No extra text, One line only.\n"
        "- No Chinese characters.\n"
        "- Convert Chinese names into Vietnamese Sino-Vietnamese readings.\n"
        // "- Never use pinyin or romanized Mandarin.\n"
        "- Keep the translation concise, natural, and suitable for on-screen subtitles.\n"
        "- Start your translation immediately on the very first line without any blank lines or prefixes.\n"
        "- Never output words like 'Vietnamese', 'Output', 'Rules', or any instruction text.\n"
    );

    if (!contextBlock.isEmpty()) {
        prompt += QStringLiteral("\nMovie context provided by user:\n") + contextBlock + QStringLiteral("\n");
    }

    if (!recentDialogueContext.isEmpty()) {
        prompt += QStringLiteral("\nContext only. Do NOT translate these lines.\n\nPrevious:\n") + recentDialogueContext + QStringLiteral("\n");
    }

    prompt += QStringLiteral("\nTranslate ONLY this line:\n") + sourceText + QStringLiteral("\n\nVietnamese:");

    // Print to debug prompt
    // qDebug() << "Translation prompt:" << prompt;

    return prompt;
}

QString repairPrompt(const QString &sourceText,
                     const QString &draftTranslation,
                     const QString &contextBlock)
{
    QString prompt = QStringLiteral(
        "Rewrite the draft into one natural Vietnamese subtitle line.\n"
        "Rules:\n"
        "- Remove all Chinese Han characters completely.\n"
        "- Use only Vietnamese/Latin script.\n"
        "- Preserve the meaning of the original Chinese source.\n"
        "- No explanations, no notes, no quotes, no extra labels.\n");

    if (!contextBlock.isEmpty()) {
        prompt += QStringLiteral("\nMovie context provided by user:\n") + contextBlock + QStringLiteral("\n");
    }

    prompt += QStringLiteral("\nOriginal Chinese:\n") + sourceText +
            //   QStringLiteral("\n\nBad draft:\n") + draftTranslation +
              QStringLiteral("\n\nOutput:");
    return prompt;
}

QString rescuePrompt(const QString &sourceText,
                    const QString &draftTranslation,
                    const QString &contextBlock,
                    const QString &recentDialogueContext)
{
    QString prompt = QStringLiteral(
        "FINAL RETRY. Output exactly one clean Vietnamese subtitle line.\n"
        "Hard constraints:\n"
        "- Do not include any Chinese characters.\n"
        "- Do not include labels like 'translation', 'dịch', '已被译为'.\n"
        "- Do not include explanations.\n"
        "- Keep it short and natural for subtitle display.\n");

    if (!contextBlock.isEmpty()) {
        prompt += QStringLiteral("\nMovie context provided by user:\n") + contextBlock + QStringLiteral("\n");
    }

    if (!recentDialogueContext.isEmpty()) {
        prompt += QStringLiteral("\nContext only. Do NOT translate these lines.\n\nPrevious:\n") + recentDialogueContext + QStringLiteral("\n");
    }

    // if (!draftTranslation.trimmed().isEmpty()) {
    //     prompt += QStringLiteral("\nBad draft to fix:\n") + draftTranslation + QStringLiteral("\n");
    // }

    prompt += QStringLiteral("\nTranslate ONLY this line:\n") + sourceText + QStringLiteral("\n\nVietnamese:");
    return prompt;
}

QString completeLinePrompt(const QString &sourceText,
                           const QString &draftTranslation,
                           const QString &contextBlock,
                           const QString &recentDialogueContext)
{
    QString prompt = QStringLiteral(
        "Rewrite into one COMPLETE Vietnamese subtitle line for the full Chinese source.\n"
        "Hard constraints:\n"
        "- Output exactly one complete sentence, not a fragment.\n"
        "- Do not output single-word fragments like 'Đã', 'Và', 'Là'.\n"
        "- Preserve key meaning and entities from the Chinese source.\n"
        "- No explanations, no notes, no labels.\n"
        "- No Chinese Han characters in output.\n");

    if (!contextBlock.isEmpty()) {
        prompt += QStringLiteral("\nMovie context provided by user:\n") + contextBlock + QStringLiteral("\n");
    }

    if (!recentDialogueContext.isEmpty()) {
        prompt += QStringLiteral("\nContext only. Do NOT translate these lines.\n\nPrevious:\n") + recentDialogueContext + QStringLiteral("\n");
    }

    // if (!draftTranslation.trimmed().isEmpty()) {
    //     prompt += QStringLiteral("\nBad fragment to avoid:\n") + draftTranslation + QStringLiteral("\n");
    // }

    prompt += QStringLiteral("\nTranslate ONLY this line:\n") + sourceText + QStringLiteral("\n\nVietnamese:");
    return prompt;
}

} // namespace TranslationTextProcessor
