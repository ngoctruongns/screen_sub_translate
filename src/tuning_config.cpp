#include "tuning_params.h"

#include <algorithm>
#include <cmath>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

// Runtime tuning configuration.
//
// Applies `config/tuning.json` over the compile-time defaults declared in tuning_params.h,
// so every value can be tuned with an app restart instead of a rebuild. The file is
// entirely optional: anything it omits — or the whole file — falls back to the default.
//
// Unknown keys are reported rather than ignored. While tuning by hand, a typo that silently
// does nothing is the most expensive kind of mistake, so it is surfaced in the log.

namespace tuning
{

namespace
{

QString g_resolvedTuningConfigPath;

QString resolveRuntimePath(const QString &rawPath)
{
    const QFileInfo directInfo(rawPath);
    if (directInfo.isAbsolute()) {
        return directInfo.absoluteFilePath();
    }

    const QString fromCwd = QFileInfo(QDir::current(), rawPath).absoluteFilePath();
    if (QFileInfo::exists(fromCwd)) {
        return fromCwd;
    }

    return QFileInfo(QCoreApplication::applicationDirPath(), rawPath).absoluteFilePath();
}

// ── Field readers ────────────────────────────────────────────────────────────
// Each one leaves the target untouched when the key is absent, so a partial config file
// overrides only what it mentions. Keys that are consumed are erased from `remaining`,
// leaving exactly the unknown keys behind for reporting.

void takeInt(QJsonObject &remaining, const QString &key, int &target, int min, int max,
             QStringList *messages)
{
    const auto it = remaining.find(key);
    if (it == remaining.end()) {
        return;
    }
    const QJsonValue value = it.value();
    remaining.erase(it);

    if (!value.isDouble()) {
        if (messages) {
            messages->append(QStringLiteral("'%1' ignored: expected a number").arg(key));
        }
        return;
    }

    const int raw = static_cast<int>(std::llround(value.toDouble()));
    const int clamped = std::clamp(raw, min, max);
    if (clamped != raw && messages) {
        messages->append(QStringLiteral("'%1' = %2 clamped to %3 (valid range %4..%5)")
                             .arg(key).arg(raw).arg(clamped).arg(min).arg(max));
    }
    target = clamped;
}

void takeDouble(QJsonObject &remaining, const QString &key, double &target, double min,
                double max, QStringList *messages)
{
    const auto it = remaining.find(key);
    if (it == remaining.end()) {
        return;
    }
    const QJsonValue value = it.value();
    remaining.erase(it);

    if (!value.isDouble()) {
        if (messages) {
            messages->append(QStringLiteral("'%1' ignored: expected a number").arg(key));
        }
        return;
    }

    const double raw = value.toDouble();
    const double clamped = std::clamp(raw, min, max);
    // Exact comparison is right here: clamp() returns `raw` itself when it is in range.
    if (clamped != raw && messages) {
        messages->append(QStringLiteral("'%1' = %2 clamped to %3 (valid range %4..%5)")
                             .arg(key).arg(raw).arg(clamped).arg(min).arg(max));
    }
    target = clamped;
}

void takeFloat(QJsonObject &remaining, const QString &key, float &target, double min,
               double max, QStringList *messages)
{
    // takeDouble leaves its target untouched when the key is absent or invalid, so the
    // round-trip through `asDouble` is a no-op in exactly those cases.
    double asDouble = static_cast<double>(target);
    takeDouble(remaining, key, asDouble, min, max, messages);
    target = static_cast<float>(asDouble);
}

void takeBool(QJsonObject &remaining, const QString &key, bool &target, QStringList *messages)
{
    const auto it = remaining.find(key);
    if (it == remaining.end()) {
        return;
    }
    const QJsonValue value = it.value();
    remaining.erase(it);

    if (!value.isBool()) {
        if (messages) {
            messages->append(QStringLiteral("'%1' ignored: expected true or false").arg(key));
        }
        return;
    }
    target = value.toBool();
}

void takeString(QJsonObject &remaining, const QString &key, QString &target, QStringList *messages)
{
    const auto it = remaining.find(key);
    if (it == remaining.end()) {
        return;
    }
    const QJsonValue value = it.value();
    remaining.erase(it);

    if (!value.isString()) {
        if (messages) {
            messages->append(QStringLiteral("'%1' ignored: expected a string").arg(key));
        }
        return;
    }
    const QString text = value.toString().trimmed();
    if (text.isEmpty()) {
        if (messages) {
            messages->append(QStringLiteral("'%1' ignored: empty string").arg(key));
        }
        return;
    }
    target = text;
}

// Keys beginning with '_' are documentation blocks (see the shipped tuning.json), never
// settings, so they are dropped before the unknown-key sweep.
void dropCommentKeys(QJsonObject &object)
{
    const QStringList keys = object.keys();
    for (const QString &key : keys) {
        if (key.startsWith(QLatin1Char('_'))) {
            object.remove(key);
        }
    }
}

void reportUnknownKeys(const QJsonObject &remaining, const QString &section, QStringList *messages)
{
    if (!messages) {
        return;
    }
    const QStringList keys = remaining.keys();
    for (const QString &key : keys) {
        messages->append(QStringLiteral("unknown key '%1' in section '%2' — ignored (typo?)")
                             .arg(key, section));
    }
}

void applyLanguageProfile(QJsonObject section, LanguageProfile &profile, const QString &name,
                          QStringList *messages)
{
    dropCommentKeys(section);

    takeString(section, QStringLiteral("recOnnxPath"), profile.recOnnxPath, messages);
    takeString(section, QStringLiteral("charsetPath"), profile.charsetPath, messages);
    takeInt(section, QStringLiteral("inputWidth"), profile.inputWidth, 32, 4096, messages);
    takeBool(section, QStringLiteral("adaptiveInputWidth"), profile.adaptiveInputWidth, messages);
    takeBool(section, QStringLiteral("autoCropSubtitleRegion"), profile.autoCropSubtitleRegion, messages);
    takeInt(section, QStringLiteral("maxTextLines"), profile.maxTextLines, 1, 8, messages);
    takeFloat(section, QStringLiteral("minOcrConfidence"), profile.minOcrConfidence, 0.0, 1.0, messages);
    takeFloat(section, QStringLiteral("edgeMinConfidence"), profile.edgeMinConfidence, 0.0, 1.0, messages);

    takeInt(section, QStringLiteral("minContentUnits"), profile.minContentUnits, 0, 100, messages);
    takeInt(section, QStringLiteral("veryShortCandidateChars"), profile.veryShortCandidateChars, 0, 200, messages);
    takeInt(section, QStringLiteral("shortCandidateChars"), profile.shortCandidateChars, 0, 400, messages);

    takeInt(section, QStringLiteral("maxIncompleteHoldUnits"), profile.maxIncompleteHoldUnits, 0, 200, messages);

    takeInt(section, QStringLiteral("historyEntryMaxChars"), profile.historyEntryMaxChars, 8, 1000, messages);

    takeDouble(section, QStringLiteral("minTranslationWordRatio"), profile.minTranslationWordRatio, 0.0, 10.0, messages);
    takeInt(section, QStringLiteral("minUnitsForRatioCheck"), profile.minUnitsForRatioCheck, 1, 200, messages);
    takeInt(section, QStringLiteral("shortSourceUnitLimit"), profile.shortSourceUnitLimit, 0, 200, messages);
    takeInt(section, QStringLiteral("longSourceUnitThreshold"), profile.longSourceUnitThreshold, 0, 400, messages);
    takeDouble(section, QStringLiteral("maxWordRatioShortSource"), profile.maxWordRatioShortSource, 0.1, 50.0, messages);
    takeDouble(section, QStringLiteral("maxWordRatioLongSource"), profile.maxWordRatioLongSource, 0.1, 50.0, messages);
    takeInt(section, QStringLiteral("maxOutputLengthFactor"), profile.maxOutputLengthFactor, 1, 200, messages);

    takeString(section, QStringLiteral("sourceSrtFileName"), profile.sourceSrtFileName, messages);

    // A short-candidate threshold below the very-short one would make the "short" rule
    // unreachable, which looks like the rule silently not working.
    if (profile.shortCandidateChars < profile.veryShortCandidateChars) {
        if (messages) {
            messages->append(
                QStringLiteral("[%1] shortCandidateChars (%2) < veryShortCandidateChars (%3); "
                               "raised to match, otherwise the 'short' rule can never apply")
                    .arg(name).arg(profile.shortCandidateChars).arg(profile.veryShortCandidateChars));
        }
        profile.shortCandidateChars = profile.veryShortCandidateChars;
    }

    reportUnknownKeys(section, name, messages);
}

void applyCapture(QJsonObject s, QStringList *m)
{
    dropCommentKeys(s);
    takeInt(s, QStringLiteral("captureIntervalMs"), kCaptureIntervalMs, 1, 5000, m);
    takeInt(s, QStringLiteral("ocrKeepaliveIntervalMs"), kOcrKeepaliveIntervalMs, 1, 60000, m);
    takeDouble(s, QStringLiteral("changeThreshold"), kChangeThreshold, 0.0, 255.0, m);
    takeDouble(s, QStringLiteral("minChangedRatio"), kMinChangedRatio, 0.0, 1.0, m);
    takeDouble(s, QStringLiteral("minStdDev"), kMinStdDev, 0.0, 255.0, m);
    reportUnknownKeys(s, QStringLiteral("capture"), m);
}

void applyOcrEngine(QJsonObject s, QStringList *m)
{
    dropCommentKeys(s);
    takeBool(s, QStringLiteral("useCudaExecutionProvider"), kUseCudaExecutionProvider, m);
    takeInt(s, QStringLiteral("paddleInputHeight"), kPaddleInputHeight, 16, 256, m);
    reportUnknownKeys(s, QStringLiteral("ocrEngine"), m);
}

void applyFilter(QJsonObject s, QStringList *m)
{
    dropCommentKeys(s);
    takeInt(s, QStringLiteral("minOcrLength"), kMinOcrLength, 1, 500, m);
    takeInt(s, QStringLiteral("minCandidateStableMs"), kMinCandidateStableMs, 0, 10000, m);
    takeInt(s, QStringLiteral("veryShortCandidateStableMs"), kVeryShortCandidateStableMs, 0, 10000, m);
    takeInt(s, QStringLiteral("shortCandidateStableMs"), kShortCandidateStableMs, 0, 10000, m);
    takeInt(s, QStringLiteral("veryShortCandidateMinFrames"), kVeryShortCandidateMinFrames, 1, 100, m);
    takeInt(s, QStringLiteral("shortCandidateMinFrames"), kShortCandidateMinFrames, 1, 100, m);
    takeInt(s, QStringLiteral("subtitleDisappearTimeoutMs"), kSubtitleDisappearTimeoutMs, 0, 60000, m);
    takeInt(s, QStringLiteral("subtitleSwitchCooldownMs"), kSubtitleSwitchCooldownMs, 0, 60000, m);
    takeInt(s, QStringLiteral("subtitleResendCooldownMs"), kSubtitleResendCooldownMs, 0, 120000, m);
    takeInt(s, QStringLiteral("recentSubtitleWindowSize"), kRecentSubtitleWindowSize, 0, 64, m);
    takeInt(s, QStringLiteral("minIncompleteHoldFrames"), kMinIncompleteHoldFrames, 1, 100, m);
    reportUnknownKeys(s, QStringLiteral("filter"), m);
}

void applyTranslation(QJsonObject s, QStringList *m)
{
    dropCommentKeys(s);
    takeDouble(s, QStringLiteral("temperature"), kTranslateTemperature, 0.0, 2.0, m);
    takeInt(s, QStringLiteral("numPredict"), kTranslateNumPredict, 1, 4096, m);
    takeInt(s, QStringLiteral("requestTimeoutMs"), kTranslateRequestTimeoutMs, 100, 600000, m);
    takeInt(s, QStringLiteral("cacheSize"), kTranslationCacheSize, 0, 10000, m);
    takeInt(s, QStringLiteral("topK"), kTranslateTopK, 0, 1000, m);
    takeDouble(s, QStringLiteral("topP"), kTranslateTopP, 0.0, 1.0, m);
    takeDouble(s, QStringLiteral("minP"), kTranslateMinP, 0.0, 1.0, m);
    takeDouble(s, QStringLiteral("repeatPenalty"), kTranslateRepeatPenalty, 0.0, 5.0, m);
    takeDouble(s, QStringLiteral("frequencyPenalty"), kTranslateFrequencyPenalty, 0.0, 5.0, m);
    takeInt(s, QStringLiteral("repeatLastN"), kTranslateRepeatLastN, 0, 8192, m);
    takeBool(s, QStringLiteral("cachePrompt"), kTranslateCachePrompt, m);
    takeInt(s, QStringLiteral("historyWindowSize"), kTranslateHistoryWindowSize, 0, 32, m);
    takeBool(s, QStringLiteral("enableRetryPasses"), kEnableRetryPasses, m);
    takeDouble(s, QStringLiteral("retryTemperature"), kTranslateRetryTemperature, 0.0, 2.0, m);
    takeInt(s, QStringLiteral("retryTopK"), kTranslateRetryTopK, 0, 1000, m);
    takeDouble(s, QStringLiteral("retryTopP"), kTranslateRetryTopP, 0.0, 1.0, m);
    takeDouble(s, QStringLiteral("retryMinP"), kTranslateRetryMinP, 0.0, 1.0, m);
    takeInt(s, QStringLiteral("lineScoreMin"), kTranslateLineScoreMin, -100000, 100000, m);
    reportUnknownKeys(s, QStringLiteral("translation"), m);
}

void applyDisplay(QJsonObject s, QStringList *m)
{
    dropCommentKeys(s);
    takeInt(s, QStringLiteral("minMs"), kDisplayMinMs, 0, 60000, m);
    takeInt(s, QStringLiteral("maxMs"), kDisplayMaxMs, 0, 60000, m);
    takeInt(s, QStringLiteral("baseMs"), kDisplayBaseMs, 0, 60000, m);
    takeInt(s, QStringLiteral("msPerChar"), kDisplayMsPerChar, 0, 5000, m);
    takeInt(s, QStringLiteral("maxLatencyMs"), kDisplayMaxLatencyMs, 0, 120000, m);
    takeInt(s, QStringLiteral("queueMaxSize"), kDisplayQueueMaxSize, 1, 1000, m);
    takeInt(s, QStringLiteral("tickMs"), kDisplayTickMs, 1, 5000, m);

    // A max below the min would clamp every entry to the max, i.e. silently ignore the min.
    if (kDisplayMaxMs < kDisplayMinMs) {
        if (m) {
            m->append(QStringLiteral("[display] maxMs (%1) < minMs (%2); raised to match")
                          .arg(kDisplayMaxMs).arg(kDisplayMinMs));
        }
        kDisplayMaxMs = kDisplayMinMs;
    }

    reportUnknownKeys(s, QStringLiteral("display"), m);
}

LanguageProfile &mutableProfileFor(SourceLanguage language)
{
    static LanguageProfile chinese = defaultChineseProfile();
    static LanguageProfile english = defaultEnglishProfile();
    return language == SourceLanguage::English ? english : chinese;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time defaults
// ─────────────────────────────────────────────────────────────────────────────

// Chinese pipeline.
//   - PP-OCRv4 server rec (default): higher accuracy than the mobile model on stylised
//     fonts / noisy backgrounds; same input (3x48xW) and same dict (ppocr_keys_v1.txt),
//     so it is a drop-in replacement — just place the .onnx file at this path.
//   - PP-OCRv4 mobile (previous): "ch_PP-OCRv4_rec_infer.onnx" + ppocr_keys_v1.txt.
//   - PP-OCRv5 server (best): "PP-OCRv5_server_rec_infer.onnx" and REQUIRES the v5 dict
//     "ppocrv5_dict.txt" instead of ppocr_keys_v1.txt (larger, multi-lang). Swap BOTH.
LanguageProfile defaultChineseProfile()
{
    LanguageProfile p;
    p.recOnnxPath = QStringLiteral("../models/paddle/ch_PP-OCRv4_rec_server_infer.onnx");
    p.charsetPath = QStringLiteral("../models/paddle/ppocr_keys_v1.txt");
    p.inputWidth = 480;
    // Left off deliberately: the Chinese pipeline is tuned and working against fixed-width
    // padding, and its edgeMinConfidence of 0.80 already suppresses the padding artifact
    // (the stray 嶺). Turn it on once ./OcrBatchEval confirms no regression.
    p.adaptiveInputWidth = false;
    p.autoCropSubtitleRegion = true;
    p.maxTextLines = 3;
    p.minOcrConfidence = 0.55f;
    p.edgeMinConfidence = 0.80f;

    p.minContentUnits = 1;
    p.veryShortCandidateChars = 1;
    p.shortCandidateChars = 3;

    p.maxIncompleteHoldUnits = 4;

    p.historyEntryMaxChars = 42;

    p.minTranslationWordRatio = 0.40;
    p.minUnitsForRatioCheck = 5;
    p.shortSourceUnitLimit = 5;
    p.longSourceUnitThreshold = 8;
    p.maxWordRatioShortSource = 4.0;
    p.maxWordRatioLongSource = 3.0;
    p.maxOutputLengthFactor = 12;

    p.sourceSrtFileName = QStringLiteral("chinese.srt");
    return p;
}

// English pipeline.
//   - PP-OCRv4 English rec + en_dict.txt (96 classes). A dedicated English model is used
//     rather than the Chinese one: the Chinese dict does contain Latin glyphs, but its
//     recognition of casing and word spacing on stylised subtitle fonts is markedly worse,
//     and spacing is semantically load-bearing in English.
//   - inputWidth is much larger than the Chinese one: an English subtitle line runs 3–4x
//     more characters than its Han equivalent, and squashing it into 480px destroys the
//     thin strokes that separate similar glyphs.
//   - minOcrConfidence sits higher than Chinese: with only ~96 classes the softmax is far
//     less diluted, so genuine reads score higher — but so does noise.
//   - edgeMinConfidence is DISABLED (0). The Chinese value exists to cut one specific
//     artifact: a dark margin in the crop decodes as a stray glyph (typically 嶺) scoring
//     far below the ~0.98 of a real Han character. English has no equivalent margin
//     artifact, and narrow but legitimate glyphs ("t", "'", ",", "l") score low enough to
//     be trimmed, which measurably cost real characters at both ends of the line on the
//     first evaluation run. Raise it only if margin noise is actually observed.
LanguageProfile defaultEnglishProfile()
{
    LanguageProfile p;
    p.recOnnxPath = QStringLiteral("../models/paddle/en_PP-OCRv4_rec_infer.onnx");
    p.charsetPath = QStringLiteral("../models/paddle/en_dict.txt");
    p.inputWidth = 800;
    p.adaptiveInputWidth = true;
    // Off: measured to cut whole leading words off English lines once the preprocessing
    // was sharpened ("We're trying to..." recognised as "ing to..."). The capture window
    // already bounds the subtitle, so the auto-crop only has downside here.
    p.autoCropSubtitleRegion = false;
    p.maxTextLines = 3;
    p.minOcrConfidence = 0.60f;
    p.edgeMinConfidence = 0.0f;

    p.minContentUnits = 1;
    p.veryShortCandidateChars = 3;
    p.shortCandidateChars = 10;

    p.maxIncompleteHoldUnits = 4;

    p.historyEntryMaxChars = 96;

    // Vietnamese renders English at roughly 1.0–1.6 words per source word, so both the
    // floor and the ceiling sit much closer to 1.0 than the Chinese ratios do.
    p.minTranslationWordRatio = 0.60;
    p.minUnitsForRatioCheck = 4;
    p.shortSourceUnitLimit = 3;
    p.longSourceUnitThreshold = 6;
    p.maxWordRatioShortSource = 3.0;
    p.maxWordRatioLongSource = 2.2;
    p.maxOutputLengthFactor = 3;

    p.sourceSrtFileName = QStringLiteral("english.srt");
    return p;
}

const LanguageProfile &profileFor(SourceLanguage language)
{
    return mutableProfileFor(language);
}

QString resolvedTuningConfigPath()
{
    return g_resolvedTuningConfigPath;
}

bool loadTuningConfig(const QString &path, QStringList *messages)
{
    g_resolvedTuningConfigPath = resolveRuntimePath(path);

    QFile file(g_resolvedTuningConfigPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (messages) {
            messages->append(QStringLiteral("no tuning file at %1 — using built-in defaults")
                                 .arg(g_resolvedTuningConfigPath));
        }
        return false;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        if (messages) {
            messages->append(QStringLiteral("tuning file %1 is not valid JSON (%2) — "
                                            "ALL built-in defaults are in effect")
                                 .arg(g_resolvedTuningConfigPath, err.errorString()));
        }
        return false;
    }

    QJsonObject root = doc.object();
    dropCommentKeys(root);

    const auto takeSection = [&root](const QString &key) -> QJsonObject {
        const auto it = root.find(key);
        if (it == root.end()) {
            return {};
        }
        const QJsonObject section = it.value().toObject();
        root.erase(it);
        return section;
    };

    applyCapture(takeSection(QStringLiteral("capture")), messages);
    applyOcrEngine(takeSection(QStringLiteral("ocrEngine")), messages);
    applyFilter(takeSection(QStringLiteral("filter")), messages);
    applyTranslation(takeSection(QStringLiteral("translation")), messages);
    applyDisplay(takeSection(QStringLiteral("display")), messages);

    applyLanguageProfile(takeSection(QStringLiteral("chinese")),
                         mutableProfileFor(SourceLanguage::Chinese),
                         QStringLiteral("chinese"), messages);
    applyLanguageProfile(takeSection(QStringLiteral("english")),
                         mutableProfileFor(SourceLanguage::English),
                         QStringLiteral("english"), messages);

    reportUnknownKeys(root, QStringLiteral("<root>"), messages);

    return true;
}

} // namespace tuning
