#include "ocr_preprocess.h"

#include <opencv2/imgproc.hpp>

namespace OcrPreprocess
{

namespace
{
constexpr double kClaheClipLimit = 2.0;
constexpr int kClaheTileSize = 8;
} // namespace

cv::Mat enhanceForRecognition(const cv::Mat &grayFrame)
{
    if (grayFrame.empty()) {
        return {};
    }

    // Denoise first: CLAHE amplifies local contrast, and amplifying sensor/compression
    // noise along with the text costs more than the blur does.
    cv::Mat denoised;
    cv::GaussianBlur(grayFrame, denoised, cv::Size(3, 3), 0.0);

    // CLAHE, not global equalization: a subtitle sits over whatever the scene happens to
    // be, so the contrast that matters is local to the text, not to the frame.
    cv::Mat claheResult;
    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(kClaheClipLimit,
                                               cv::Size(kClaheTileSize, kClaheTileSize));
    clahe->apply(denoised, claheResult);

    // Unsharp masking sharpens glyph edges, which is what keeps thin strokes from being
    // swallowed when the crop is later scaled down to the model's 48px height.
    cv::Mat blurred;
    cv::GaussianBlur(claheResult, blurred, cv::Size(5, 5), 1.0);

    cv::Mat sharpened;
    cv::addWeighted(claheResult, 1.5, blurred, -0.5, 0, sharpened);

    cv::Mat normalized;
    cv::normalize(sharpened, normalized, 0, 255, cv::NORM_MINMAX);

    return normalized;
}

} // namespace OcrPreprocess
