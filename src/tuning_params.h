#pragma once

namespace tuning
{

    struct NoiseProfileConfig {
        double changeThreshold;  // Threshold for detecting changes in the image.
        double minChangedRatio;  // Minimum ratio of changed pixels to consider as a change.
        double minStdDev;        // Minimum standard deviation of pixel values to consider as a change.
        int minOcrLength;        // Minimum length of OCR text to consider for dispatching
        int requiredRepeatCount; // Number of consecutive identical OCR results required to consider it stable.
        int minOcrAcceptGapMs;   // Minimum time in milliseconds between accepting the same OCR text again.
    };

    // PaddleOCR ONNX Runtime (C++) configuration.
    inline constexpr bool kUseCudaExecutionProvider = true;
    inline constexpr const char *kPaddleRecOnnxPath = "../models/paddle/ch_PP-OCRv4_rec_infer.onnx";
    inline constexpr const char *kPaddleCharsetPath = "../models/paddle/ppocr_keys_v1.txt";

    // Local translation model defaults (fast realtime): Helsinki-NLP/opus-mt-zh-vi.
    inline constexpr const char *kTranslateModelDir    = "../models/translate";
    inline constexpr int64_t     kTranslateDecoderStartId = 65000;
    inline constexpr int64_t     kTranslateEosId           = 0;
    inline constexpr int64_t     kTranslateSourceLangId    = -1; // no forced source lang token
    inline constexpr int64_t     kTranslateTargetLangId    = -1; // no forced target lang token
    inline constexpr int         kTranslateRuntimeMaxDecodeSteps = 120;
    inline constexpr int         kTranslateRuntimeNumBeams = 6;
    inline constexpr int         kTranslateRuntimeMaxInputTokens = 64;
    inline constexpr int         kTranslateMaxDecodeSteps  = 120;
    inline constexpr int         kTranslateNumBeams        = 6;
    inline constexpr double      kTranslateLengthPenalty   = 1.0;
    inline constexpr int kPaddleInputHeight = 48;
    inline constexpr int kPaddleInputWidth = 320;

    // Preset profile values.
    inline constexpr NoiseProfileConfig kFastProfile = {
        1.55, // changeThreshold
        0.006,
        8.0,
        1,
        1,
        25,
    };

    inline constexpr NoiseProfileConfig kBalancedProfile = {
        1.65, // changeThreshold
        0.009,
        8.5,
        1,
        2,
        30,
    };

    inline constexpr NoiseProfileConfig kCleanProfile = {
        2.20, // changeThreshold
        0.015,
        13.0,
        1,
        2,
        100,
    };

    // Default startup profile.
    inline constexpr NoiseProfileConfig kDefaultProfile = kBalancedProfile;

    // Backward-compatible single-value defaults used by worker initialization.
    inline constexpr double kChangeThreshold = kDefaultProfile.changeThreshold;
    inline constexpr double kMinChangedRatio = kDefaultProfile.minChangedRatio;
    inline constexpr double kMinStdDev = kDefaultProfile.minStdDev;

    // Subtitle dispatch dedupe (avoid translating the same on-screen subtitle repeatedly).
    inline constexpr int kSubtitleDisappearTimeoutMs = 800;  // Time after which an empty OCR result is considered a subtitle disappearance.
    inline constexpr int kSubtitleSwitchCooldownMs = 300;    // Minimum time between dispatching different subtitles.
    inline constexpr int kSubtitleResendCooldownMs = 2200;   // Minimum time before resending the same subtitle.

    // Number of recent subtitles to track for deduplication.
    inline constexpr int kRecentSubtitleWindowSize = 4;

} // namespace tuning
