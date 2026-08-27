#pragma once

#include <opencv2/core.hpp>

namespace OcrPreprocess
{

// The image enhancement applied to a grayscale crop before it reaches the OCR engine:
// denoise -> CLAHE -> unsharp mask -> min-max normalize.
//
// Shared deliberately. The live capture path and the offline evaluator MUST run the exact
// same transformation, otherwise a confidence threshold measured by OcrBatchEval is not the
// threshold that applies at runtime, and an accuracy figure from the evaluator says nothing
// about what the overlay actually shows. They diverged once already — the evaluator was
// upscaling 2.6x and skipping CLAHE and the unsharp mask entirely, which are exactly the
// steps that rescue thin strokes ('l', 'i', 't') in subtitle fonts.
//
// Returns an empty Mat for empty input. The caller keeps whatever gating it needs
// (CaptureWorker applies a std-dev contrast gate on the result).
cv::Mat enhanceForRecognition(const cv::Mat &grayFrame);

} // namespace OcrPreprocess
