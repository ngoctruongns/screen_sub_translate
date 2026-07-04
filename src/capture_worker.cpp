#include "capture_worker.h"

#include <QGuiApplication>
#include <QImage>
#include <QMutexLocker>
#include <QPixmap>
#include <QScreen>
#include <QThread>
#include <QDateTime>
#include <QDir>
#include <QDebug>

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

CaptureWorker::CaptureWorker(const QRect &scanZone, QObject *parent)
    : QObject(parent), scanZone_(scanZone)
{
}

void CaptureWorker::start()
{
    running_.store(true);
    qint64 lastOcrDispatchMs = 0;

    while (running_.load()) {
        const qint64 cycleStart = QDateTime::currentMSecsSinceEpoch();
        double changeThreshold;
        double minChangedRatio;
        {
            QMutexLocker locker(&zoneMutex_);
            changeThreshold = changeThreshold_;
            minChangedRatio = minChangedRatio_;
        }

        const cv::Mat gray = grabGrayFrame();

        if (!gray.empty()) {

            cv::Mat small;
            cv::resize(gray, small, cv::Size(), 0.25, 0.25, cv::INTER_AREA);

            cv::Mat previousSmallCopy;
            {
                QMutexLocker locker(&zoneMutex_);
                previousSmallCopy = previousSmallGray_.clone();
                previousSmallGray_ = small.clone();
            }

            bool shouldRunOcr = false;

            if (previousSmallCopy.empty()) {
                // Frame first time
                shouldRunOcr = true;
            } else {

                cv::Mat diff;
                cv::absdiff(previousSmallCopy, small, diff);

                const double diffMean = cv::mean(diff)[0];

                cv::Mat diffMask;
                cv::threshold(diff, diffMask, 12, 255, cv::THRESH_BINARY);

                const double changedRatio = static_cast<double>(cv::countNonZero(diffMask)) /
                                            static_cast<double>(diffMask.total());

                shouldRunOcr = (diffMean >= changeThreshold) && (changedRatio >= minChangedRatio);

                // qDebug()
                //     << "diffMean =" << diffMean
                //     << "changedRatio =" << changedRatio
                //     << "OCR =" << shouldRunOcr;
            }

            if (!shouldRunOcr) {
                const bool keepaliveDue =
                    (lastOcrDispatchMs == 0) ||
                    ((cycleStart - lastOcrDispatchMs) >= tuning::kOcrKeepaliveIntervalMs);
                shouldRunOcr = keepaliveDue;
            }

            if (shouldRunOcr) {

                const cv::Mat processed = preprocessForOcr(gray);

                if (!processed.empty()) {
                    emit imageProcessed(processed);
                    lastOcrDispatchMs = cycleStart;
                }
            }
        }

        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - cycleStart;

        const int remain = tuning::kCaptureIntervalMs - static_cast<int>(elapsed);

        if (remain > 0) {
            QThread::msleep(remain);
        }
    }

    {
        QMutexLocker locker(&zoneMutex_);
        previousSmallGray_.release();
    }
    running_.store(false);
}

void CaptureWorker::stop()
{ running_.store(false); }

void CaptureWorker::setScanZone(const QRect &scanZone)
{
    QMutexLocker locker(&zoneMutex_);
    scanZone_ = scanZone;
    previousSmallGray_.release();
}

void CaptureWorker::setNoiseParams(double changeThreshold, double minChangedRatio, double minStdDev)
{
    QMutexLocker locker(&zoneMutex_);
    changeThreshold_ = changeThreshold;
    minChangedRatio_ = minChangedRatio;
    minStdDev_ = minStdDev;
}

cv::Mat CaptureWorker::grabGrayFrame() const
{
    QRect zone;
    {
        QMutexLocker locker(&zoneMutex_);
        zone = scanZone_;
    }

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

cv::Mat CaptureWorker::preprocessForOcr(const cv::Mat &grayFrame) const
{
    if (grayFrame.empty()) {
        return {};
    }

    // Step 1: Gaussian blur for noise reduction
    cv::Mat denoised;
    cv::GaussianBlur(grayFrame, denoised, cv::Size(3, 3), 0.0);

    // Step 2: Resize to OCR input size
    cv::Mat enlarged;
    cv::resize(denoised, enlarged, cv::Size(), 2.6, 2.6, cv::INTER_CUBIC);

    // Step 3: Apply CLAHE (Contrast Limited Adaptive Histogram Equalization)
    // to improve text contrast and visibility
    cv::Mat claheResult = enlarged.clone();
    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
    clahe->apply(enlarged, claheResult);

    // Step 4: Unsharp masking to sharpen text edges
    cv::Mat blurred;
    cv::GaussianBlur(claheResult, blurred, cv::Size(5, 5), 1.0);
    cv::Mat sharpened = claheResult.clone();
    cv::addWeighted(claheResult, 1.5, blurred, -0.5, 0, sharpened);

    // Step 5: Normalize to standard OCR input range
    cv::Mat normalized;
    cv::normalize(sharpened, normalized, 0, 255, cv::NORM_MINMAX);

    // Step 6: Check standard deviation threshold
    double minStdDev = minStdDev_;
    {
        QMutexLocker locker(&zoneMutex_);
        minStdDev = minStdDev_;
    }

    cv::Scalar meanVal;
    cv::Scalar stdDev;
    cv::meanStdDev(normalized, meanVal, stdDev);

    if (stdDev[0] < minStdDev) {
        return {};
    }

    // Debug: Save preprocessed images for analysis (every 50th frame)
    static int frameCounter = 0;
    static int savedCounter = 0;
    QString debugDir = QString::fromUtf8("test/image/debug_preprocessed");
    QDir dir;
    if (!dir.exists(debugDir)) {
        dir.mkpath(debugDir);
    }
    dir.setPath(debugDir);

    // Remove all old files in the debug directory
    if (savedCounter % 10 == 0) {
        const QStringList oldFiles = dir.entryList(QStringList() << "*.png", QDir::Files);
        for (const QString &file : oldFiles) {
            dir.remove(file);
        }
    }

    if (++frameCounter % 10 == 0) {
        ++savedCounter;
        QString filename = QString::fromUtf8("%1/frame_%2_preprocessed.png")
            .arg(debugDir).arg(frameCounter, 6, 10, QChar('0'));

        cv::imwrite(filename.toStdString(), normalized);
        // qDebug() << "[PREPROCESS DEBUG]" << filename << "stdDev=" << stdDev[0];
    }

    return normalized.clone();
}
