#pragma once

// Centralized tuning constants, ordered to follow the runtime pipeline:
//   Capture -> OCR engine -> OCR subtitle filter -> Translation (AI model) -> Display.

namespace tuning
{

    // ─────────────────────────────────────────────────────────────────────────
    // 1. CAPTURE (screen grab + change/noise gate)  — capture_worker.cpp
    // ─────────────────────────────────────────────────────────────────────────
    inline constexpr int kCaptureIntervalMs = 50;          // Capture worker loop interval.
    inline constexpr int kOcrKeepaliveIntervalMs = 220;    // Force an OCR dispatch even without frame change, at least this often.

    // Frame-diff / noise gate deciding whether a captured frame is worth OCR-ing
    // (single fixed configuration, equivalent to the previous Balanced profile).
    inline constexpr double kChangeThreshold = 1.65;       // Mean frame-diff threshold.
    inline constexpr double kMinChangedRatio = 0.009;      // Minimum changed-pixel ratio.
    inline constexpr double kMinStdDev = 8.5;              // Reject low-contrast frames below this std dev.

    // ─────────────────────────────────────────────────────────────────────────
    // 2. OCR ENGINE (PaddleOCR ONNX Runtime, C++)  — ocr_engine.cpp
    // ─────────────────────────────────────────────────────────────────────────
    inline constexpr bool kUseCudaExecutionProvider = true; // Try CUDA EP first; falls back to CPU with a warning.

    // Recognition model + its matching character dictionary. These MUST come as a pair.
    //   - PP-OCRv4 server (default below): higher accuracy than the mobile model on stylised
    //     fonts / noisy backgrounds; same input (3x48xW) and same dict (ppocr_keys_v1.txt),
    //     so it is a drop-in replacement — just place the .onnx file at this path.
    //   - PP-OCRv4 mobile (previous): "ch_PP-OCRv4_rec_infer.onnx" + ppocr_keys_v1.txt.
    //   - PP-OCRv5 server (best): "PP-OCRv5_server_rec_infer.onnx" and REQUIRES the v5 dict
    //     "ppocrv5_dict.txt" instead of ppocr_keys_v1.txt (larger, multi-lang). Swap BOTH.
    // ~10-60 MB on disk; negligible VRAM next to the translation model.
    inline constexpr const char *kPaddleRecOnnxPath = "../models/paddle/ch_PP-OCRv4_rec_server_infer.onnx";
    inline constexpr const char *kPaddleCharsetPath = "../models/paddle/ppocr_keys_v1.txt";
    inline constexpr int kPaddleInputHeight = 48;          // Recognition input height (fixed).
    inline constexpr int kPaddleInputWidth = 480;          // Recognition input width (padded). Wider = less horizontal
                                                           // squashing on long Han lines. Requires a dynamic-width ONNX
                                                           // export (most PP-OCRv4 rec exports are); revert to 320 if the
                                                           // model rejects the input shape.

    // Reject a recognition whose mean per-character confidence (max softmax prob over
    // the CTC-emitted steps) is below this. Filters garbled reads (e.g. "一居鳞") before
    // they reach translation. TUNE ON REAL FOOTAGE: confidence is logged per detection;
    // raise toward 0.8 to drop more garbage, lower if valid subtitles get dropped.
    inline constexpr float kMinOcrConfidence = 0.55f;

    // ─────────────────────────────────────────────────────────────────────────
    // 3. OCR SUBTITLE FILTER (stabilization)  — ocr_subtitle_filter.cpp
    // ─────────────────────────────────────────────────────────────────────────
    inline constexpr int kMinOcrLength = 1;                // Minimum accepted candidate length.
    inline constexpr int kMinCandidateStableMs = 150;      // Base stable time before a candidate can dispatch.
    inline constexpr int kMinHanCharsForCandidate = 1;     // Minimum Han chars for a candidate to be considered.

    // Dynamic stability for short candidates: shorter text needs longer/steadier confirmation.
    // Tuned P1: allow shorter text with lower frame/time requirements for faster detection.
    inline constexpr int kVeryShortCandidateStableMs = 350; // len <= 1: reduced from 520ms for faster detection.
    inline constexpr int kShortCandidateStableMs = 150;     // len <= 3: reduced from 260ms for faster detection.
    inline constexpr int kVeryShortCandidateMinFrames = 2;  // len <= 1: reduced from 3 for shorter text.
    inline constexpr int kShortCandidateMinFrames = 1;      // len <= 3: reduced from 2 for shorter text.

    // Subtitle dispatch dedupe / lifecycle (avoid translating the same on-screen subtitle repeatedly).
    inline constexpr int kSubtitleDisappearTimeoutMs = 1000; // Empty OCR longer than this => subtitle disappeared.
    inline constexpr int kSubtitleSwitchCooldownMs = 200;   // Minimum time between dispatching different subtitles.
    inline constexpr int kSubtitleResendCooldownMs = 2200;  // Minimum time before resending the same subtitle.
    inline constexpr int kRecentSubtitleWindowSize = 4;     // Recent subtitle keys / translation history window for dedupe.

    // ─────────────────────────────────────────────────────────────────────────
    // 4. TRANSLATION — AI MODEL / BACKEND  — translate_client.cpp, translation_backend_adapter.cpp
    // ─────────────────────────────────────────────────────────────────────────
    // Local translation backend defaults (overridable via the JSON config file).
    inline constexpr const char *kTranslateBackendConfigPath = "../translate/translation_backend.json";
    inline constexpr const char *kTranslateBaseUrl = "http://127.0.0.1:8080";
    inline constexpr const char *kTranslateModel = "";       // Optional. Empty => discover/use backend default when possible.
    inline constexpr const char *kTranslateApiMode = "auto"; // auto | llamacpp | openai | ollama
    inline constexpr const char *kTranslateContextFilePath = "../translate/movie_context.txt";
    inline constexpr const char *kTranslateGlossaryFilePath = "../translate/glossary.json";

    // First-pass generation.
    inline constexpr double kTranslateTemperature = 0.01;   // Sampling temperature: 0.0 = greedy; keep <=0.1 for translation.
    inline constexpr int kTranslateNumPredict = 64;         // Max new tokens per response (~64 covers most subtitle lines).
    inline constexpr int kTranslateRequestTimeoutMs = 15000; // Abort a stalled request so a frozen backend can't wedge the pipeline.
    inline constexpr int kTranslationCacheSize = 96;        // LRU cache of successful translations keyed by source line.

    // Prompt context and recent-dialogue history.
    inline constexpr int kTranslatePromptContextMaxChars = 900; // Max chars loaded from movie_context.txt.
    inline constexpr int kTranslateHistoryWindowSize = 2;       // Number of recent source lines injected as context.
    inline constexpr int kTranslateHistoryEntryMaxCharsHan = 42; // Truncation for a Chinese history entry.
    inline constexpr int kTranslateHistoryEntryMaxCharsVie = 82; // Truncation for a Vietnamese history entry.

    // Single retry pass: if the first output fails quality checks, retry once.
    // Disabling drops bad responses outright — useful for latency testing but hurts translation quality.
    // NOTE: kTranslateTemperature / kTranslateNumPredict / kTranslateRetryTemperature are DEFAULTS only;
    // they can be overridden at runtime via translation_backend.json (temperature / numPredict / retryTemperature).
    inline constexpr bool kEnableRetryPasses = true;
    inline constexpr double kTranslateRetryTemperature = 0.3; // Looser than first pass so retry can escape a degenerate first output.
    inline constexpr int kTranslateRetryTopK = 30;
    inline constexpr double kTranslateRetryTopP = 0.8;
    inline constexpr double kTranslateRetryMinP = 0.1;

    // ─────────────────────────────────────────────────────────────────────────
    // 5. TRANSLATION — QUALITY CHECKS  — translation_text_processor.cpp
    // ─────────────────────────────────────────────────────────────────────────
    inline constexpr int kTranslateLineScoreMin = 0;       // Minimum score for selectBestVietnameseLine() to accept a line.

    // Ratio-based short-translation guard: suspicious when (output word count / source Han count) < ratio,
    // applied only when the source has at least kMinHanCharsForRatioCheck Han chars.
    inline constexpr double kMinTranslationWordRatio = 0.40;
    inline constexpr int kMinHanCharsForRatioCheck = 5;

    // ─────────────────────────────────────────────────────────────────────────
    // 6. DISPLAY QUEUE  — overlay_window.cpp
    // ─────────────────────────────────────────────────────────────────────────
    // Display duration is clamped to [kDisplayMinMs, kDisplayMaxMs].
    // Formula: clamp(kDisplayBaseMs + charCount * kDisplayMsPerChar, min, max)
    inline constexpr int kDisplayMinMs        = 300;   // Minimum display time per entry (ms).
    inline constexpr int kDisplayMaxMs        = 3000;  // Maximum display time per entry (ms).
    inline constexpr int kDisplayBaseMs       = 250;   // Base display time before per-char contribution (ms).
    inline constexpr int kDisplayMsPerChar    = 70;    // Additional ms per displayed character.
    inline constexpr int kDisplayMaxLatencyMs = 2500;  // Drop entry if it has been queued longer than this (ms).
    inline constexpr int kDisplayQueueMaxSize = 5;     // Max queue depth before overflow handling.
    inline constexpr int kDisplayTickMs       = 60;    // Timer interval for advancing the display queue (ms).

} // namespace tuning
