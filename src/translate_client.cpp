#include "translate_client.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include "tuning_params.h"

namespace
{

enum class LocalApiMode {
    Auto,
    LlamaCpp,
    OpenAI,
    Ollama,
};

struct GlossaryLoadResult {
    QString promptLines;
    QVector<QPair<QString, QString>> aliasPairs;
};

QString resolveRuntimePath(const QString &relative)
{
    const QFileInfo direct(relative);
    if (direct.isAbsolute()) {
        return direct.absoluteFilePath();
    }

    const QString fromCwd = QFileInfo(QDir::current(), relative).absoluteFilePath();
    if (QFileInfo::exists(fromCwd)) {
        return fromCwd;
    }

    return QFileInfo(QCoreApplication::applicationDirPath(), relative).absoluteFilePath();
}

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

QString loadPromptContext(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QString text = QString::fromUtf8(file.readAll()).trimmed();
    if (text.size() > tuning::kLlamaPromptContextMaxChars) {
        text = text.left(tuning::kLlamaPromptContextMaxChars).trimmed();
    }
    return text;
}

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

// Loads glossary.json and returns:
// 1) prompt lines as "term -> canonical"
// 2) alias replacement rules for post-translation normalization
GlossaryLoadResult loadGlossary(const QString &path)
{
    GlossaryLoadResult result;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return result;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "TranslateClient: failed to parse glossary JSON:" << err.errorString();
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

    // Print to debug
    qDebug() << "[POST-PROCESS] Original translation:" << text;

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

    return text.trimmed();
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

QString normalizedBaseUrl(QString url)
{
    while (url.endsWith('/')) {
        url.chop(1);
    }
    return url;
}

QUrl llamaGenerateUrl(const QString &baseUrl)
{
    QString normalized = normalizedBaseUrl(baseUrl);
    if (normalized.endsWith(QStringLiteral("/api/generate"))) {
        return QUrl(normalized);
    }
    if (normalized.endsWith(QStringLiteral("/api"))) {
        normalized += QStringLiteral("/generate");
    } else {
        normalized += QStringLiteral("/api/generate");
    }
    return QUrl(normalized);
}

QUrl openAiChatCompletionsUrl(const QString &baseUrl)
{
    QString normalized = normalizedBaseUrl(baseUrl);
    if (normalized.endsWith(QStringLiteral("/v1/chat/completions"))) {
        return QUrl(normalized);
    }
    if (normalized.endsWith(QStringLiteral("/v1"))) {
        normalized += QStringLiteral("/chat/completions");
    } else {
        normalized += QStringLiteral("/v1/chat/completions");
    }
    return QUrl(normalized);
}

QUrl llamaCppCompletionUrl(const QString &baseUrl)
{
    QString normalized = normalizedBaseUrl(baseUrl);
    if (normalized.endsWith(QStringLiteral("/completion"))) {
        return QUrl(normalized);
    }
    normalized += QStringLiteral("/completion");
    return QUrl(normalized);
}

LocalApiMode parseLocalApiMode(const QString &modeText)
{
    const QString mode = modeText.trimmed().toLower();
    if (mode == QStringLiteral("llamacpp") || mode == QStringLiteral("llama.cpp")) {
        return LocalApiMode::LlamaCpp;
    }
    if (mode == QStringLiteral("openai") || mode == QStringLiteral("chat")) {
        return LocalApiMode::OpenAI;
    }
    if (mode == QStringLiteral("ollama")) {
        return LocalApiMode::Ollama;
    }
    return LocalApiMode::Auto;
}

LocalApiMode resolvedLocalApiMode(const QString &modeText, const QString &baseUrl)
{
    const LocalApiMode parsed = parseLocalApiMode(modeText);
    if (parsed != LocalApiMode::Auto) {
        return parsed;
    }

    const QString url = normalizedBaseUrl(baseUrl).toLower();
    if (url.contains(QStringLiteral(":11434")) || url.contains(QStringLiteral("/api"))) {
        return LocalApiMode::Ollama;
    }
    if (url.contains(QStringLiteral("/v1"))) {
        return LocalApiMode::OpenAI;
    }

    return LocalApiMode::LlamaCpp;
}

QString localApiModeName(LocalApiMode mode)
{
    switch (mode) {
    case LocalApiMode::LlamaCpp:
        return QStringLiteral("llamacpp");
    case LocalApiMode::OpenAI:
        return QStringLiteral("openai");
    case LocalApiMode::Ollama:
        return QStringLiteral("ollama");
    case LocalApiMode::Auto:
    default:
        return QStringLiteral("auto");
    }
}

QUrl localEndpointUrl(const QString &baseUrl, LocalApiMode mode)
{
    switch (mode) {
    case LocalApiMode::Ollama:
        return llamaGenerateUrl(baseUrl);
    case LocalApiMode::OpenAI:
        return openAiChatCompletionsUrl(baseUrl);
    case LocalApiMode::LlamaCpp:
    case LocalApiMode::Auto:
    default:
        return llamaCppCompletionUrl(baseUrl);
    }
}

QString extractLocalResponseText(const QJsonObject &root, LocalApiMode mode)
{
    if (mode == LocalApiMode::Ollama) {
        QString text = root.value(QStringLiteral("response")).toString().trimmed();
        if (text.isEmpty()) {
            text = root.value(QStringLiteral("message")).toObject().value(QStringLiteral("content")).toString().trimmed();
        }
        return text;
    }

    if (mode == LocalApiMode::LlamaCpp) {
        QString text = root.value(QStringLiteral("content")).toString().trimmed();
        if (!text.isEmpty()) {
            return text;
        }
    }

    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (!choices.isEmpty() && choices.first().isObject()) {
        const QJsonObject first = choices.first().toObject();
        QString text = first.value(QStringLiteral("text")).toString().trimmed();
        if (!text.isEmpty()) {
            return text;
        }

        text = first.value(QStringLiteral("message")).toObject().value(QStringLiteral("content")).toString().trimmed();
        if (!text.isEmpty()) {
            return text;
        }
    }

    QString fallback = root.value(QStringLiteral("response")).toString().trimmed();
    if (!fallback.isEmpty()) {
        return fallback;
    }
    return root.value(QStringLiteral("content")).toString().trimmed();
}

QString translationPrompt(const QString &sourceText,
                         const QString &contextBlock,
                         const QString &recentDialogueContext)
{
    QString prompt = QStringLiteral(
        "You are translating OCR subtitles for a Chinese historical war film into natural Vietnamese subtitle style.\n"
        "Rules:\n"
        "- Return ONLY the Vietnamese subtitle.\n"
        "- Absolutely NO explanations, NO notes, NO labels, No extra text, One line only.\n"
        "- Never copy any Chinese Han characters into the answer.\n"
        "- If the source contains person names, place names, or military titles, render them in Vietnamese-friendly Latin script (Sino-Vietnamese readings).\n"
        "- If OCR is noisy, infer the intended Chinese sentence before translating.\n"
        "- Keep the translation concise, natural, and suitable for on-screen subtitles.\n"
        "- Start your translation immediately on the very first line without any blank lines or prefixes.\n"
        "- Never output words like 'Vietnamese', 'Output', 'Rules', or any instruction text.\n"
        "- Avoid repeating words or phrases within the same sentence unless grammatically required. Do not reuse rigid phrasing from previous lines if a more natural synonym exists.\n"
    );

    if (!contextBlock.isEmpty()) {
        prompt += QStringLiteral("\nMovie context provided by user:\n") + contextBlock + QStringLiteral("\n");
    }

    if (!recentDialogueContext.isEmpty()) {
        prompt += QStringLiteral("\nRecent Chinese subtitles (for narrative context only):\n") + recentDialogueContext + QStringLiteral("\n");
    }

    prompt += QStringLiteral("\nChinese OCR subtitle:\n") + sourceText + QStringLiteral("\n\nOutput:");
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
              QStringLiteral("\n\nBad draft:\n") + draftTranslation +
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
        prompt += QStringLiteral("\nRecent Chinese subtitles (for narrative context only):\n") + recentDialogueContext + QStringLiteral("\n");
    }

    if (!draftTranslation.trimmed().isEmpty()) {
        prompt += QStringLiteral("\nBad draft to fix:\n") + draftTranslation + QStringLiteral("\n");
    }

    prompt += QStringLiteral("\nChinese OCR subtitle:\n") + sourceText + QStringLiteral("\n\nOutput:");
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
        prompt += QStringLiteral("\nRecent Chinese subtitles (for narrative context only):\n") + recentDialogueContext + QStringLiteral("\n");
    }

    if (!draftTranslation.trimmed().isEmpty()) {
        prompt += QStringLiteral("\nBad fragment to avoid:\n") + draftTranslation + QStringLiteral("\n");
    }

    prompt += QStringLiteral("\nChinese OCR subtitle:\n") + sourceText + QStringLiteral("\n\nOutput:");
    return prompt;
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
    const int outChars = translatedText.trimmed().size();

    if (srcHan >= 8 && outWords <= 1) {
        return true;
    }
    if (srcHan >= 10 && outWords <= 2) {
        return true;
    }
    if (srcHan >= 8 && outChars <= 4) {
        return true;
    }
    return false;
}

bool isShortSourcePhrase(const QString &sourceText)
{
    const int srcHan = hanCharCount(sourceText);
    return srcHan > 0 && srcHan <= 5;
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

} // namespace

TranslateClient::TranslateClient(QObject *parent)
    : QObject(parent),
      networkManager_(new QNetworkAccessManager(this))
{
    connect(networkManager_, &QNetworkAccessManager::finished, this, &TranslateClient::onReplyFinished);

    initializeLocalBackend();
}

TranslateClient::~TranslateClient() = default;

void TranslateClient::initializeLocalBackend()
{
    llamaBaseUrl_ = QString::fromUtf8(qgetenv("SST_LLAMA_BASE_URL")).trimmed();
    if (llamaBaseUrl_.isEmpty()) {
        llamaBaseUrl_ = QString::fromUtf8(tuning::kLlamaBaseUrl);
    }

    llamaModel_ = QString::fromUtf8(qgetenv("SST_LLAMA_MODEL")).trimmed();
    if (llamaModel_.isEmpty()) {
        llamaModel_ = QString::fromUtf8(tuning::kLlamaModel);
    }

    llmApiMode_ = QString::fromUtf8(qgetenv("SST_LLM_API")).trimmed().toLower();
    if (llmApiMode_.isEmpty()) {
        llmApiMode_ = QString::fromUtf8(tuning::kLlamaApiMode);
    }

    promptContextFilePath_ = QString::fromUtf8(qgetenv("SST_TRANSLATE_CONTEXT_FILE")).trimmed();
    if (promptContextFilePath_.isEmpty()) {
        promptContextFilePath_ = resolveRuntimePath(QString::fromUtf8(tuning::kLlamaContextFilePath));
    }

    glossaryFilePath_ = QString::fromUtf8(qgetenv("SST_TRANSLATE_GLOSSARY_FILE")).trimmed();
    if (glossaryFilePath_.isEmpty()) {
        glossaryFilePath_ = resolveRuntimePath(QString::fromUtf8(tuning::kLlamaGlossaryFilePath));
    }

    const LocalApiMode mode = resolvedLocalApiMode(llmApiMode_, llamaBaseUrl_);
    localInitialized_ = !llamaModel_.isEmpty() && localEndpointUrl(llamaBaseUrl_, mode).isValid();
    glossaryAliasPairs_.clear();
    if (localInitialized_) {
        cachedContextBlock_ = loadPromptContext(promptContextFilePath_);
        const GlossaryLoadResult glossaryData = loadGlossary(glossaryFilePath_);
        glossaryAliasPairs_ = glossaryData.aliasPairs;
        const QString glossaryBlock = glossaryData.promptLines;
        if (!glossaryBlock.isEmpty()) {
            if (!cachedContextBlock_.isEmpty()) {
                cachedContextBlock_ += QStringLiteral("\n\nTerm glossary (apply when relevant):\n") + glossaryBlock;
            } else {
                cachedContextBlock_ = QStringLiteral("Term glossary (apply when relevant):\n") + glossaryBlock;
            }
        }
        qDebug() << "TranslateClient: local Llama backend ready"
                 << "model=" << llamaModel_ << "mode=" << localApiModeName(mode)
                 << "url=" << localEndpointUrl(llamaBaseUrl_, mode).toString()
                 << "context=" << promptContextFilePath_
                 << "glossary=" << glossaryFilePath_
                 << "aliasRules=" << glossaryAliasPairs_.size();
    } else {
        qWarning() << "TranslateClient: local Llama backend config invalid"
                   << "model=" << llamaModel_ << "baseUrl=" << llamaBaseUrl_ << "mode=" << llmApiMode_;
    }
}

QString TranslateClient::applyGlossaryAliasNormalization(const QString &translatedText) const
{
    if (translatedText.isEmpty() || glossaryAliasPairs_.isEmpty()) {
        return translatedText;
    }

    QString out = translatedText;
    for (const auto &pair : glossaryAliasPairs_) {
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

void TranslateClient::requestTranslation(const QString &sourceText)
{
    const QString normalized = sourceText.trimmed();
    if (normalized.isEmpty()) {
        return;
    }

    if (!isLikelyChineseSubtitle(normalized)) {
        emit translationError(QStringLiteral("Skipped non-Chinese OCR candidate"));
        return;
    }

    if (activeReply_) {
        if (normalized != inFlightText_) {
            pendingText_ = normalized;
        }
        return;
    }

    if (!localInitialized_) {
        emit translationError(QStringLiteral("Local Llama backend not initialized"));
        return;
    }

    startLlamaRequest(normalized);
}

QString TranslateClient::recentDialogueContext() const
{
    if (recentTranslationHistory_.isEmpty()) {
        return {};
    }

    const int historySize = static_cast<int>(recentTranslationHistory_.size());
    QStringList lines;
    const int startIndex = std::max(0, historySize - tuning::kLlamaHistoryWindowSize);
    lines.reserve(historySize - startIndex);
    for (int i = startIndex; i < historySize; ++i) {
        const TranslationContextEntry &entry = recentTranslationHistory_.at(i);
        // Only include the Chinese source lines, NOT the Vietnamese translations.
        // Feeding translated text back risks propagating any dirty/garbage output
        // from a previous pass as a "correct" example, creating a feedback loop.
        lines.append(shortText(entry.sourceText, tuning::kLlamaHistoryEntryMaxCharsHan));
    }

    return lines.join(QStringLiteral(", "));
}

void TranslateClient::rememberTranslationContext(const QString &sourceText,
                                                 const QString &translatedText)
{
    if (sourceText.trimmed().isEmpty() || translatedText.trimmed().isEmpty()) {
        return;
    }

    if (!recentTranslationHistory_.isEmpty()) {
        const TranslationContextEntry &last = recentTranslationHistory_.constLast();
        if (last.sourceText == sourceText && last.translatedText == translatedText) {
            return;
        }
    }

    recentTranslationHistory_.push_back({sourceText, translatedText});
    while (recentTranslationHistory_.size() > tuning::kRecentSubtitleWindowSize) {
        recentTranslationHistory_.removeFirst();
    }
}

void TranslateClient::startLlamaRequest(const QString &sourceText)
{
    const QString dialogueContext = recentDialogueContext();
    startLlamaPromptRequest(sourceText,
                            translationPrompt(sourceText, cachedContextBlock_, dialogueContext),
                            std::nullopt,
                            false,
                            false);
}

void TranslateClient::startLlamaPromptRequest(const QString &sourceText,
                                              const QString &prompt,
                                              const std::optional<QString> &draftTranslation,
                                              bool isRepairPass,
                                              bool isRescuePass)
{
    inFlightText_ = sourceText;

    const LocalApiMode mode = resolvedLocalApiMode(llmApiMode_, llamaBaseUrl_);
    const QUrl endpoint = localEndpointUrl(llamaBaseUrl_, mode);
    QNetworkRequest request(endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QJsonObject payload;
    if (mode == LocalApiMode::Ollama) {
        payload.insert(QStringLiteral("model"), llamaModel_);
        payload.insert(QStringLiteral("prompt"), prompt);
        payload.insert(QStringLiteral("stream"), false);

        QJsonObject options;
        options.insert(QStringLiteral("temperature"), tuning::kLlamaTemperature);
        options.insert(QStringLiteral("num_predict"), tuning::kLlamaNumPredict);
        options.insert(QStringLiteral("stop"), QJsonArray{QStringLiteral("\n\n"), QStringLiteral("###")});
        payload.insert(QStringLiteral("options"), options);
    } else if (mode == LocalApiMode::OpenAI) {
        payload.insert(QStringLiteral("model"), llamaModel_);
        payload.insert(QStringLiteral("temperature"), tuning::kLlamaTemperature);
        payload.insert(QStringLiteral("max_tokens"), tuning::kLlamaNumPredict);

        QJsonArray messages;
        QJsonObject userMsg;
        userMsg.insert(QStringLiteral("role"), QStringLiteral("user"));
        userMsg.insert(QStringLiteral("content"), prompt);
        messages.append(userMsg);
        payload.insert(QStringLiteral("messages"), messages);
        payload.insert(QStringLiteral("stream"), false);
        payload.insert(QStringLiteral("stop"), QJsonArray{QStringLiteral("\n\n"), QStringLiteral("###")});
    } else {
        payload.insert(QStringLiteral("prompt"), prompt);
        payload.insert(QStringLiteral("temperature"), tuning::kLlamaTemperature);
        payload.insert(QStringLiteral("n_predict"), tuning::kLlamaNumPredict); // max token output
        payload.insert(QStringLiteral("stream"), false);    // Disable streaming for simplicity
        payload.insert(QStringLiteral("cache_prompt"), true); // Allow model to cache prompt for faster subsequent calls

        // Add Llama-specific tuning parameters for better subtitle translation quality.
        payload.insert(QStringLiteral("repeat_penalty"), tuning::kLlamaRepeatPenalty);
        payload.insert(QStringLiteral("frequency_penalty"), tuning::kLlamaFrequencyPenalty);
        payload.insert(QStringLiteral("repeat_last_n"), tuning::kLlamaRepeatLastN);

        payload.insert(QStringLiteral("stop"),
                       QJsonArray{
                           // Avoid single-newline stop because many models emit '\n' first.
                           // That can terminate generation before any real text is produced.
                           QStringLiteral("\n\n"),
                           QStringLiteral("<|im_end|>"),    // Token end of chat for Qwen
                           QStringLiteral("<|endoftext|>"), // Token end of text for Qwen
                           QStringLiteral("###")            // End of chat for Ollama
                       });
    }

    QNetworkReply *reply =
        networkManager_->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    reply->setProperty("sourceText", sourceText);
    reply->setProperty("localApiMode", localApiModeName(mode));
    reply->setProperty("repairPass", isRepairPass);
    reply->setProperty("rescuePass", isRescuePass);
    if (draftTranslation.has_value()) {
        reply->setProperty("draftTranslation", *draftTranslation);
    }
    activeReply_ = reply;
}

void TranslateClient::onReplyFinished(QNetworkReply *reply)
{
    const LocalApiMode localMode = parseLocalApiMode(reply->property("localApiMode").toString());
    const QString sourceText = reply->property("sourceText").toString();
    const bool isRepairPass = reply->property("repairPass").toBool();
    const bool isRescuePass = reply->property("rescuePass").toBool();

    if (activeReply_ == reply) {
        activeReply_.clear();
    }
    inFlightText_.clear();

    if (reply->error() == QNetworkReply::OperationCanceledError) {
        reply->deleteLater();
    } else if (reply->error() != QNetworkReply::NoError) {
        emit translationError(reply->errorString());
        reply->deleteLater();
    } else {
        QJsonParseError parseError;
        const QByteArray responseData = reply->readAll();
        const QJsonDocument document = QJsonDocument::fromJson(responseData, &parseError);

        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            emit translationError(QStringLiteral("Failed to parse local Llama response"));
            reply->deleteLater();
        } else {
            const QJsonObject root = document.object();
            QString rawTranslated = extractLocalResponseText(root, localMode);

            QString translated = sanitizeFinalTranslation(rawTranslated);
            translated = postProcessTranslation(translated);
            translated = applyGlossaryAliasNormalization(translated);

            if (isSuspiciouslyShortTranslation(sourceText, translated) && !isRescuePass) {
                const QString dialogueContext = recentDialogueContext();
                reply->deleteLater();
                startLlamaPromptRequest(sourceText,
                                        completeLinePrompt(sourceText,
                                                           translated,
                                                           cachedContextBlock_,
                                                           dialogueContext),
                                        translated,
                                        false,
                                        true);
                return;
            }

            if (translated.isEmpty()) {
                qWarning() << "TranslateClient: empty local translation after sanitize"
                           << "source=" << shortText(sourceText, 40)
                           << "raw=" << shortText(rawTranslated, 80)
                           << "mode=" << localApiModeName(localMode)
                           << "repairPass=" << isRepairPass
                           << "rescuePass=" << isRescuePass;
                if (tuning::kEnableRetryPasses && !isRescuePass) {
                    const QString dialogueContext = recentDialogueContext();
                    reply->deleteLater();
                    startLlamaPromptRequest(sourceText,
                                            rescuePrompt(sourceText,
                                                         rawTranslated,
                                                         cachedContextBlock_,
                                                         dialogueContext),
                                            rawTranslated,
                                            false,
                                            true);
                    return;
                }
                emit translationError(QStringLiteral("Local Llama translation is empty"));
            } else if (containsHanCharacters(translated) && !isRepairPass) {
                if (tuning::kEnableRetryPasses) {
                    reply->deleteLater();
                    startLlamaPromptRequest(sourceText,
                                            repairPrompt(sourceText, translated, cachedContextBlock_),
                                            translated,
                                            true,
                                            false);
                    return;
                }
                emit translationError(QStringLiteral("Local Llama output contains Han"));
            } else if (containsHanCharacters(translated)) {
                if (tuning::kEnableRetryPasses && !isRescuePass) {
                    const QString dialogueContext = recentDialogueContext();
                    reply->deleteLater();
                    startLlamaPromptRequest(sourceText,
                                            rescuePrompt(sourceText,
                                                         translated,
                                                         cachedContextBlock_,
                                                         dialogueContext),
                                            translated,
                                            false,
                                            true);
                    return;
                }
                emit translationError(QStringLiteral("Local Llama output still contains Han after repair"));
            } else if (isEnglishHeavyOutput(translated)) {
                emit translationError(QStringLiteral("Local Llama output rejected: English-heavy output"));
            } else if (isOverExpandedTranslation(sourceText, translated)) {
                emit translationError(QStringLiteral("Local Llama output rejected: over-expanded short source translation"));
            } else if (isSuspiciouslyShortTranslation(sourceText, translated)) {
                emit translationError(QStringLiteral("Local Llama output rejected: suspiciously short translation"));
            } else {
                rememberTranslationContext(sourceText, translated);
                emit translationReady(translated, sourceText);
            }

            reply->deleteLater();
        }
    }

    if (!pendingText_.isEmpty()) {
        const QString next = pendingText_;
        pendingText_.clear();
        requestTranslation(next);
    }
}
