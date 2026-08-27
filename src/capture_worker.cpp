#include "capture_worker.h"

#include "ocr_preprocess.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QImage>
#include <QMutexLocker>
#include <QPixmap>
#include <QScreen>

#ifdef SST_DEBUG_BUILD
#include <QDir>
#include <opencv2/imgcodecs.hpp>
#endif

#include <algorithm>
#include <cmath>

#include <opencv2/imgproc.hpp>

namespace
{
constexpr double kSmallFrameScale = 0.25;
constexpr double kDiffPixelThreshold = 12.0;
constexpr int kDebugSaveEveryNFrames = 50;
constexpr int kDebugCleanupEveryNSaves = 10;

double sanitizeNonNegative(double value, double fallback)
{
    return std::isfinite(value) ? std::max(0.0, value) : fallback;
}
} // namespace

CaptureWorker::CaptureWorker(const QRect &scanZone, QObject *parent)
    : QObject(parent)
{
    params_.scanZone = scanZone;
}

CaptureWorker::WorkerParams CaptureWorker::paramsSnapshot() const
{
    QMutexLocker locker(&paramsMutex_);
    return params_;
}

void CaptureWorker::start()
{
    if (running_.exchange(true)) {
        return;
    }

    qint64 lastOcrDispatchMs = 0;

    while (running_.load()) {
        const qint64 cycleStart = QDateTime::currentMSecsSinceEpoch();
        const WorkerParams params = paramsSnapshot();
        const cv::Mat gray = grabGrayFrame(params.scanZone);

        if (!gray.empty()) {
            cv::Mat small;
            cv::resize(gray, small, cv::Size(), kSmallFrameScale, kSmallFrameScale, cv::INTER_AREA);

            bool shouldRunOcr = previousSmallGray_.empty();

            if (!shouldRunOcr) {
                cv::Mat diff;
                cv::absdiff(previousSmallGray_, small, diff);

                const double diffMean = cv::mean(diff)[0];

                cv::Mat diffMask;
                cv::threshold(diff, diffMask, kDiffPixelThreshold, 255, cv::THRESH_BINARY);

                const double changedRatio = static_cast<double>(cv::countNonZero(diffMask)) /
                                            static_cast<double>(diffMask.total());

                shouldRunOcr = (diffMean >= params.changeThreshold) &&
                               (changedRatio >= params.minChangedRatio);
            }

            previousSmallGray_ = std::move(small);

            if (!shouldRunOcr) {
                const bool keepaliveDue =
                    (lastOcrDispatchMs == 0) ||
                    ((cycleStart - lastOcrDispatchMs) >= tuning::kOcrKeepaliveIntervalMs);
                shouldRunOcr = keepaliveDue;
            }

            if (shouldRunOcr) {
                const cv::Mat processed = preprocessForOcr(gray, params.minStdDev);

                if (!processed.empty()) {
                    emit imageProcessed(processed);
                    lastOcrDispatchMs = cycleStart;
                }
            }
        }

        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - cycleStart;
        const int remain = tuning::kCaptureIntervalMs - static_cast<int>(elapsed);

        if (remain > 0) {
            // Process any pending queued-connection calls (e.g. setScanZone) while waiting.
            QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, remain);
        }
    }

    previousSmallGray_.release();
    running_.store(false);
}

void CaptureWorker::stop()
{
    running_.store(false);
}

void CaptureWorker::setScanZone(const QRect &scanZone)
{
    QMutexLocker locker(&paramsMutex_);
    params_.scanZone = scanZone;
    previousSmallGray_.release();
}

void CaptureWorker::setNoiseParams(double changeThreshold, double minChangedRatio, double minStdDev)
{
    QMutexLocker locker(&paramsMutex_);
    params_.changeThreshold = sanitizeNonNegative(changeThreshold, tuning::kChangeThreshold);
    params_.minChangedRatio =
        std::clamp(std::isfinite(minChangedRatio) ? minChangedRatio : tuning::kMinChangedRatio, 0.0, 1.0);
    params_.minStdDev = sanitizeNonNegative(minStdDev, tuning::kMinStdDev);
}

cv::Mat CaptureWorker::grabGrayFrame(const QRect &zone) const
{
    if (!zone.isValid() || zone.width() < 2 || zone.height() < 2) {
        return {};
    }

    QScreen *screen = QGuiApplication::screenAt(zone.center());
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (!screen) {
        return {};
    }

    const QPixmap frame = screen->grabWindow(0, zone.x(), zone.y(), zone.width(), zone.height());
    if (frame.isNull()) {
        return {};
    }

    const QImage grayImage = frame.toImage().convertToFormat(QImage::Format_Grayscale8);
    cv::Mat grayMat(grayImage.height(), grayImage.width(), CV_8UC1,
                    const_cast<uchar *>(grayImage.bits()), grayImage.bytesPerLine());
    return grayMat.clone();
}

cv::Mat CaptureWorker::preprocessForOcr(const cv::Mat &grayFrame, double minStdDev) const
{
    if (grayFrame.empty()) {
        return {};
    }

    // Shared with the offline evaluator so both see identical pixels — see ocr_preprocess.h.
    const cv::Mat normalized = OcrPreprocess::enhanceForRecognition(grayFrame);
    if (normalized.empty()) {
        return {};
    }

    // Contrast gate: reject frames that carry no legible text at all.
    cv::Scalar meanVal;
    cv::Scalar stdDev;
    cv::meanStdDev(normalized, meanVal, stdDev);

    if (stdDev[0] < minStdDev) {
        return {};
    }

    // Optional: Save debug images for analysis if enabled with Debug build flag
#ifdef SST_DEBUG_BUILD
    static int frameCounter = 0;
    static int savedCounter = 0;
    const QString debugDir = QString::fromUtf8("logs/debug_preprocessed");

    QDir dir;
    if (!dir.exists(debugDir)) {
        dir.mkpath(debugDir);
    }
    dir.setPath(debugDir);

    if (savedCounter > 0 && savedCounter % kDebugCleanupEveryNSaves == 0) {
        const QStringList oldFiles = dir.entryList(QStringList() << "*.png", QDir::Files);
        for (const QString &file : oldFiles) {
            dir.remove(file);
        }
    }

    if (++frameCounter % kDebugSaveEveryNFrames == 1) {
        ++savedCounter;
        const QString filename = QString::fromUtf8("%1/frame_%2_preprocessed.png")
            .arg(debugDir)
            .arg(frameCounter, 6, 10, QChar('0'));

        cv::imwrite(filename.toStdString(), normalized);
    }
#endif

    return normalized;
}
