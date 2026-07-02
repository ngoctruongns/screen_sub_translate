#pragma once

namespace tuning
{

    struct NoiseProfileConfig {
        double changeThreshold;  // Threshold for detecting changes in the image.
        double minChangedRatio;  // Minimum ratio of changed pixels to consider as a change.
        double minStdDev;        // Minimum standard deviation of pixel values to consider as a change.
        int minOcrLength;        // Minimum length of OCR text to consider for dispatching
        int minCandidateStableMs;// Minimum time in Ms that a candidate OCR text must remain stable
    };

    // Candidate quality gate and dynamic stability.
    // Tuned P1: Allow shorter text with lower frame/time requirements for better detection
    inline constexpr int kMinHanCharsForCandidate = 1;
    inline constexpr int kVeryShortCandidateStableMs = 350; // len <= 1: reduced from 520ms for faster detection
    inline constexpr int kShortCandidateStableMs = 150;     // len <= 3: reduced from 260ms for faster detection
    inline constexpr int kVeryShortCandidateMinFrames = 2;  // len <= 1: reduced from 3 for shorter text
    inline constexpr int kShortCandidateMinFrames = 1;      // len <= 3: reduced from 2 for shorter text

    // PaddleOCR ONNX Runtime (C++) configuration.
    inline constexpr bool kUseCudaExecutionProvider = true;
    inline constexpr const char *kPaddleRecOnnxPath = "../models/paddle/ch_PP-OCRv4_rec_infer.onnx";
    inline constexpr const char *kPaddleCharsetPath = "../models/paddle/ppocr_keys_v1.txt";

    // Local translation backend defaults via llama.cpp server.
    inline constexpr const char *kLlamaBaseUrl = "http://127.0.0.1:8080";
    inline constexpr const char *kLlamaModel = "qwen2.5:7b-instruct-q4_K_M";
    inline constexpr const char *kLlamaApiMode = "llamacpp"; // auto | llamacpp | openai | ollama
    inline constexpr const char *kLlamaContextFilePath = "../translate/movie_context.txt";
    inline constexpr double kLlamaTemperature = 0.05;
    inline constexpr int kLlamaNumPredict = 48;
    inline constexpr int kLlamaPromptContextMaxChars = 900;
    inline constexpr int kLlamaHistoryWindowSize = 2;
    inline constexpr int kLlamaHistoryEntryMaxChars = 42;
    inline constexpr int kTranslationCacheSize = 96;
    inline constexpr bool kEnableRetryPasses = false;

    // Backward-compatibility constants kept for evaluation tools still using legacy ONNX translator.
    inline constexpr const char *kTranslateModelDir = "../models/translate";
    inline constexpr int64_t kTranslateDecoderStartId = 65000;
    inline constexpr int64_t kTranslateEosId = 0;
    inline constexpr int64_t kTranslateSourceLangId = -1;
    inline constexpr int64_t kTranslateTargetLangId = -1;
    inline constexpr int kTranslateRuntimeMaxDecodeSteps = 120;
    inline constexpr int kTranslateRuntimeNumBeams = 6;
    inline constexpr int kTranslateRuntimeMaxInputTokens = 64;
    inline constexpr int kTranslateMaxDecodeSteps = 120;
    inline constexpr int kTranslateNumBeams = 6;
    inline constexpr double kTranslateLengthPenalty = 1.0;
    inline constexpr int kPaddleInputHeight = 48;
    inline constexpr int kPaddleInputWidth = 320;

    // Preset profile values.
    // P1 Tuning: Reduced minCandidateStableMs for faster subtitle detection
    inline constexpr NoiseProfileConfig kFastProfile = {
        1.55, // changeThreshold
        0.006,
        8.0,
        1,
        140,  // minCandidateStableMs: reduced from 180
    };

    inline constexpr NoiseProfileConfig kBalancedProfile = {
        1.65, // changeThreshold
        0.009,
        8.5,
        1,
        150,  // minCandidateStableMs: reduced from 190
    };

    inline constexpr NoiseProfileConfig kCleanProfile = {
        2.20, // changeThreshold
        0.015,
        13.0,
        1,
        180,  // minCandidateStableMs: reduced from 230
    };

    // Default startup profile.
    inline constexpr NoiseProfileConfig kDefaultProfile = kBalancedProfile;

    // Backward-compatible single-value defaults used by worker initialization.
    inline constexpr double kChangeThreshold = kDefaultProfile.changeThreshold;
    inline constexpr double kMinChangedRatio = kDefaultProfile.minChangedRatio;
    inline constexpr double kMinStdDev = kDefaultProfile.minStdDev;

    // Subtitle dispatch dedupe (avoid translating the same on-screen subtitle repeatedly).
    inline constexpr int kSubtitleDisappearTimeoutMs = 800;  // Time after which an empty OCR result is considered a subtitle disappearance.
    inline constexpr int kSubtitleSwitchCooldownMs = 200;    // Minimum time between dispatching different subtitles.
    inline constexpr int kSubtitleResendCooldownMs = 2200;   // Minimum time before resending the same subtitle.

    // Number of recent subtitles to track for deduplication.
    inline constexpr int kRecentSubtitleWindowSize = 4;

    // Translation display queue parameters.
    // Display duration is clamped to [kDisplayMinMs, kDisplayMaxMs].
    // Formula: clamp(kDisplayBaseMs + charCount * kDisplayMsPerChar, min, max)
    inline constexpr int kDisplayMinMs        = 200;   // Minimum display time per entry (ms)
    inline constexpr int kDisplayMaxMs        = 3000;  // Maximum display time per entry (ms)
    inline constexpr int kDisplayBaseMs       = 200;   // Base display time before per-char contribution (ms)
    inline constexpr int kDisplayMsPerChar    = 50;    // Additional ms per displayed character
    inline constexpr int kDisplayMaxLatencyMs = 2500;  // Drop entry if it has been queued longer than this (ms)
    inline constexpr int kDisplayQueueMaxSize = 5;     // Max queue depth before overflow handling
    inline constexpr int kDisplayTickMs       = 60;    // Timer interval for advancing the display queue (ms)

    // Capture interval for the capture worker thread.
    inline constexpr int kCaptureIntervalMs = 50;
    inline constexpr int kOcrKeepaliveIntervalMs = 220;

} // namespace tuning
