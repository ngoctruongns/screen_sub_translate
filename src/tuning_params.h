#pragma once

#include <QString>
#include <QStringList>

#include "source_language.h"

// Centralized tuning parameters, ordered to follow the runtime pipeline:
//   Capture -> OCR engine -> OCR subtitle filter -> Translation (AI model) -> Display.
//
// EVERY value here is a DEFAULT that can be overridden at runtime from `config/tuning.json`,
// so tuning a value needs an app restart but never a rebuild. loadTuningConfig() applies the
// file over these defaults; anything the file omits keeps the default below.
//
// Values that are the same for every source language are free variables in this namespace.
// Values that differ per language live in LanguageProfile, one profile per source language,
// selected at runtime by profileFor().
//
// THREADING: these are plain mutable globals, written once by loadTuningConfig() at startup
// and only read afterwards. Call it from main() BEFORE any worker thread is created; there
// is no locking, and it is not safe to reload while the pipeline is running.

namespace tuning
{

    // ─────────────────────────────────────────────────────────────────────────
    // 1. CAPTURE (screen grab + change/noise gate)  — capture_worker.cpp
    // ─────────────────────────────────────────────────────────────────────────
    inline int kCaptureIntervalMs = 50;          // Capture worker loop interval.
    inline int kOcrKeepaliveIntervalMs = 220;    // Force an OCR dispatch even without frame change, at least this often.

    // Frame-diff / noise gate deciding whether a captured frame is worth OCR-ing.
    inline double kChangeThreshold = 1.65;       // Mean frame-diff threshold.
    inline double kMinChangedRatio = 0.009;      // Minimum changed-pixel ratio.
    inline double kMinStdDev = 8.5;              // Reject low-contrast frames below this std dev.

    // ─────────────────────────────────────────────────────────────────────────
    // 2. OCR ENGINE (PaddleOCR ONNX Runtime, C++)  — ocr_engine.cpp
    // ─────────────────────────────────────────────────────────────────────────
    inline bool kUseCudaExecutionProvider = true; // Try CUDA EP first; falls back to CPU with a warning.
    inline int kPaddleInputHeight = 48;           // Recognition input height. 48 for PP-OCRv4/v5; some older models use 32.

    // The recognition model, its charset, the input width and the confidence gates are
    // all per-language — see LanguageProfile below.

    // ─────────────────────────────────────────────────────────────────────────
    // 3. OCR SUBTITLE FILTER (stabilization)  — ocr_subtitle_filter.cpp
    // ─────────────────────────────────────────────────────────────────────────
    inline int kMinOcrLength = 1;                // Minimum accepted candidate length.
    inline int kMinCandidateStableMs = 150;      // Base stable time before a candidate can dispatch.

    // Dynamic stability for short candidates: shorter text needs longer/steadier confirmation.
    // The *length* at which a candidate counts as "short" is per-language (a Han line carries
    // roughly four times the meaning per character that an English line does) — see
    // LanguageProfile::veryShortCandidateChars / shortCandidateChars.
    inline int kVeryShortCandidateStableMs = 350; // Very short text: longer confirmation window.
    inline int kShortCandidateStableMs = 150;     // Short text.
    inline int kVeryShortCandidateMinFrames = 2;  // Very short text: minimum stable frames.
    inline int kShortCandidateMinFrames = 1;      // Short text: minimum stable frames.

    // Subtitle dispatch dedupe / lifecycle (avoid translating the same on-screen subtitle repeatedly).
    inline int kSubtitleDisappearTimeoutMs = 1000; // Empty OCR longer than this => subtitle disappeared.
    inline int kSubtitleSwitchCooldownMs = 200;   // Minimum time between dispatching different subtitles.
    inline int kSubtitleResendCooldownMs = 2200;  // Minimum time before resending the same subtitle.
    inline int kRecentSubtitleWindowSize = 4;     // Recent subtitle keys / translation history window for dedupe.

    // Incomplete-subtitle hold (a line ending in a comma is held to be merged with its
    // continuation). Only SHORT lead-in clauses (connectives / adverbials) really need
    // merging; a long clause ending in a comma is already a self-contained unit and is
    // dispatched immediately. This also stops a spurious trailing comma (OCR margin
    // hallucination) on a long line from delaying translation.
    // The length limit is per-language (LanguageProfile::maxIncompleteHoldUnits).
    //
    // A hold only happens after the incomplete read is stable across at least this many
    // frames, so a one-frame comma flicker never triggers one.
    inline int kMinIncompleteHoldFrames = 2;

    // ─────────────────────────────────────────────────────────────────────────
    // 4. TRANSLATION — AI MODEL / BACKEND  — translate_client.cpp, translation_backend_adapter.cpp
    // ─────────────────────────────────────────────────────────────────────────
    // Bootstrap paths. These are NOT part of config/tuning.json — they say where the other
    // config files live, so they have to be known before any config is read.
    // config/ is flat: a file that serves one pipeline stage is named after that stage
    // (translation_*.json); tuning.json is shared and is divided into per-stage sections
    // instead.
    inline constexpr const char *kTuningConfigPath = "../config/tuning.json";
    inline constexpr const char *kTranslateBackendConfigPath = "../config/translation_backend.json";

    // Backend *connection* defaults, overridable via translation_backend.json. That file
    // answers "which backend, where, and with which files"; every knob that shapes the
    // OUTPUT lives in tuning.json instead, so there is exactly one place to tune.
    inline constexpr const char *kTranslateBaseUrl = "http://127.0.0.1:8080";
    inline constexpr const char *kTranslateModel = "";       // Optional. Empty => discover/use backend default when possible.
    inline constexpr const char *kTranslateApiMode = "auto"; // auto | llamacpp | openai | ollama
    inline constexpr const char *kTranslateGlossaryFilePath = "../config/translation_glossary_zh.json";
    inline constexpr const char *kTranslateGlossaryFilePathEn = "../config/translation_glossary_en.json";

    // First-pass generation.
    inline double kTranslateTemperature = 0.01;   // Sampling temperature: 0.0 = greedy; keep <=0.1 for translation.
    inline int kTranslateNumPredict = 64;         // Max new tokens per response (~64 covers most subtitle lines).
    inline int kTranslateRequestTimeoutMs = 15000; // Abort a stalled request so a frozen backend can't wedge the pipeline.
    inline int kTranslationCacheSize = 96;        // LRU cache of successful translations keyed by source line.

    // First-pass sampling filters, applied in order (topK -> topP -> minP).
    inline int kTranslateTopK = 20;               // Keep only the top-K most probable tokens. 0 = disabled.
    inline double kTranslateTopP = 0.8;           // Nucleus sampling cutoff; 0.8–0.95 balances quality vs. determinism.
    inline double kTranslateMinP = 0.1;           // Discard tokens with prob < minP x best_prob. 0.05–0.10 suppresses stray Han.

    // Repetition control, applied over the last kTranslateRepeatLastN tokens.
    inline double kTranslateRepeatPenalty = 1.05;   // Multiplier on tokens already seen. 1.0 = disabled.
    inline double kTranslateFrequencyPenalty = 1.15; // Extra penalty scaling with occurrence count.
    inline int kTranslateRepeatLastN = 128;          // Look-back window (tokens) for the repeat penalty.

    // Ask the backend to cache the prompt prefix across calls (llama.cpp). Cuts latency
    // because the rules block at the front of every prompt is identical.
    inline bool kTranslateCachePrompt = true;

    // Recent-dialogue history injected as prompt context.
    inline int kTranslateHistoryWindowSize = 2;   // Number of recent source lines injected as context.

    // Single retry pass: if the first output fails quality checks, retry once.
    // Disabling drops bad responses outright — useful for latency testing but hurts translation quality.
    inline bool kEnableRetryPasses = true;
    inline double kTranslateRetryTemperature = 0.3; // Looser than first pass so retry can escape a degenerate first output.
    inline int kTranslateRetryTopK = 30;
    inline double kTranslateRetryTopP = 0.8;
    inline double kTranslateRetryMinP = 0.1;

    // ─────────────────────────────────────────────────────────────────────────
    // 5. TRANSLATION — QUALITY CHECKS  — translation_text_processor.cpp
    // ─────────────────────────────────────────────────────────────────────────
    inline int kTranslateLineScoreMin = 0;       // Minimum score for selectBestVietnameseLine() to accept a line.

    // The length-ratio guards compare the Vietnamese output word count against the
    // *source unit count* — Han characters for Chinese, words for English. Both the
    // ratios and the unit thresholds are per-language, since one Han character carries
    // far more meaning than one English word. See LanguageProfile below.

    // ─────────────────────────────────────────────────────────────────────────
    // 6. DISPLAY QUEUE  — overlay_window.cpp
    // ─────────────────────────────────────────────────────────────────────────
    // Display duration is clamped to [kDisplayMinMs, kDisplayMaxMs].
    // Formula: clamp(kDisplayBaseMs + charCount * kDisplayMsPerChar, min, max)
    inline int kDisplayMinMs        = 300;   // Minimum display time per entry (ms).
    inline int kDisplayMaxMs        = 3500;  // Maximum display time per entry (ms).
    inline int kDisplayBaseMs       = 250;   // Base display time before per-char contribution (ms).
    inline int kDisplayMsPerChar    = 70;    // Additional ms per displayed character.
    inline int kDisplayMaxLatencyMs = 2500;  // Drop entry if it has been queued longer than this (ms).
    inline int kDisplayQueueMaxSize = 5;     // Max queue depth before overflow handling.
    inline int kDisplayTickMs       = 60;    // Timer interval for advancing the display queue (ms).

    // ─────────────────────────────────────────────────────────────────────────
    // 7. PER-LANGUAGE PIPELINE PROFILES
    // ─────────────────────────────────────────────────────────────────────────
    // Everything that genuinely differs between the Chinese and the English pipeline.
    // Selected once per language switch by profileFor(); nothing else in the codebase
    // should branch on SourceLanguage for a *value* — only for behaviour.
    //
    // "Source units" is the language-neutral measure of how much meaning the source
    // line carries: Han characters for Chinese, whitespace-delimited words for English.
    // All translation length guards are expressed against it.
    struct LanguageProfile
    {
        // ── OCR engine ────────────────────────────────────────────────────────
        // Recognition model + its matching character dictionary. These MUST come as a pair:
        // a model paired with the wrong dictionary decodes to garbage.
        QString recOnnxPath;
        QString charsetPath;
        // Recognition input width. With adaptiveInputWidth this is the MAXIMUM (long lines
        // are squashed into it); otherwise every crop is padded out to exactly this width.
        // Wider = less horizontal squashing, at a proportional inference cost. Requires a
        // dynamic-width ONNX export.
        int inputWidth = 480;
        // Size the input tensor to the text instead of always filling inputWidth. The tail
        // of a fixed-width canvas is black padding that the CTC decoder still scans, and it
        // can emit spurious characters there. Sizing to the text removes that dead zone and
        // makes inference cheaper on short lines.
        bool adaptiveInputWidth = true;
        // Auto-tighten the crop onto the detected text before recognition (Otsu + contour
        // merge). This is a safety net for a loosely-placed capture window, and it is a
        // heuristic: it merges only the contours that pass its size filters, so anything it
        // rejects is CUT AWAY. On high-contrast input, adjacent glyphs close into one tall
        // blob that can fail those filters, and a whole leading word disappears before the
        // model ever sees it. Leave off when the capture window is placed tightly.
        bool autoCropSubtitleRegion = true;
        // Reject a recognition whose mean per-character confidence is below this.
        // TUNE ON REAL FOOTAGE: confidence is logged per detection.
        float minOcrConfidence = 0.55f;
        // Trim leading/trailing recognized characters whose individual confidence is below
        // this — dark margins that slip into the crop decode as a stray low-confidence edge
        // character. Set to 0 to disable.
        float edgeMinConfidence = 0.80f;

        // ── OCR subtitle filter ───────────────────────────────────────────────
        int minContentUnits = 1;          // Minimum source units for a read to be a candidate at all.
        int veryShortCandidateChars = 1;  // Candidate length (chars) at or below which "very short" stability rules apply.
        int shortCandidateChars = 3;      // ...and "short" stability rules.

        // ── Incomplete-subtitle hold ──────────────────────────────────────────
        int maxIncompleteHoldUnits = 4;   // Hold on a trailing comma only up to this many source units.

        // ── Translation prompt / history ──────────────────────────────────────
        int historyEntryMaxChars = 42;    // Truncation for one recent source line injected as context.

        // ── Translation quality gate ──────────────────────────────────────────
        // Output is judged as (Vietnamese word count / source unit count).
        double minTranslationWordRatio = 0.40; // Below this => suspiciously short.
        int    minUnitsForRatioCheck = 5;      // Ratio is too noisy under this many source units; a hard floor is used instead.
        int    shortSourceUnitLimit = 5;       // At or below this many units the source is a "short phrase" (over-expansion check).
        int    longSourceUnitThreshold = 8;    // Above this many units the looser long-source max ratio applies.
        double maxWordRatioShortSource = 4.0;  // Above this ratio a short source is over-generated.
        double maxWordRatioLongSource = 3.0;   // ...and a long one.
        int    maxOutputLengthFactor = 12;     // Output chars > sourceChars * this => clearly over-generated.

        // ── Logging ───────────────────────────────────────────────────────────
        QString sourceSrtFileName = QStringLiteral("chinese.srt"); // Source-side .srt written next to vietnamese.srt.
    };

    // Compile-time defaults, before config/tuning.json is applied.
    LanguageProfile defaultChineseProfile();
    LanguageProfile defaultEnglishProfile();

    // The live profile for a language. Reads are lock-free; see the THREADING note above.
    const LanguageProfile &profileFor(SourceLanguage language);

    // Applies config/tuning.json over the defaults. Returns false when the file is missing
    // or unparseable — in which case every default above stays in effect, which is a valid
    // configuration, so a missing file is not fatal. `messages` collects human-readable
    // notes (unknown keys, out-of-range values that were clamped) for logging.
    bool loadTuningConfig(const QString &path, QStringList *messages = nullptr);

    // Absolute path loadTuningConfig() last resolved, for logging and error messages.
    QString resolvedTuningConfigPath();

} // namespace tuning
