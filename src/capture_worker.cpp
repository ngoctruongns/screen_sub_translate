#include "capture_worker.h"

#include <QGuiApplication>
#include <QImage>
#include <QMutexLocker>
#include <QPixmap>
#include <QScreen>
#include <QThread>

#include <opencv2/imgproc.hpp>

CaptureWorker::CaptureWorker(const QRect &scanZone, QObject *parent)
    : QObject(parent), scanZone_(scanZone)
{
}

void CaptureWorker::start()
{
    running_.store(true);
    int sleepMs = minIntervalMs_;

    while (running_.load()) {
        double changeThreshold = changeThreshold_;
        double minChangedRatio = minChangedRatio_;
        {
            QMutexLocker locker(&zoneMutex_);
            changeThreshold = changeThreshold_;
            minChangedRatio = minChangedRatio_;
        }

        const cv::Mat gray = grabGrayFrame();
        if (gray.empty()) {
            QThread::msleep(maxIntervalMs_);
            continue;
        }

        cv::Mat small;
        cv::resize(gray, small, cv::Size(), 0.25, 0.25, cv::INTER_AREA);

        cv::Mat previousSmallCopy;
        {
            QMutexLocker locker(&zoneMutex_);
            previousSmallCopy = previousSmallGray_.clone();
        }

        if (previousSmallCopy.empty()) {
            {
                QMutexLocker locker(&zoneMutex_);
                previousSmallGray_ = small.clone();
            }
            sleepMs = std::min(maxIntervalMs_, minIntervalMs_ + 24);
            QThread::msleep(static_cast<unsigned long>(sleepMs));
            continue;
        }

        cv::Mat diff;
        cv::absdiff(previousSmallCopy, small, diff);
        const double diffMean = cv::mean(diff)[0];
        cv::Mat diffMask;
        cv::threshold(diff, diffMask, 12, 255, cv::THRESH_BINARY);
        const double changedRatio =
            static_cast<double>(cv::countNonZero(diffMask)) / static_cast<double>(diffMask.total());
        {
            QMutexLocker locker(&zoneMutex_);
            previousSmallGray_ = small.clone();
        }

        if (diffMean < changeThreshold || changedRatio < minChangedRatio) {
            sleepMs = std::min(maxIntervalMs_, sleepMs + 12);
            QThread::msleep(static_cast<unsigned long>(sleepMs));
            continue;
        }

        const cv::Mat processed = preprocessForOcr(gray);
        if (!processed.empty()) {
            emit imageProcessed(processed);
        }
        sleepMs = minIntervalMs_;
        QThread::msleep(static_cast<unsigned long>(sleepMs));
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

    cv::Mat denoised;
    cv::GaussianBlur(grayFrame, denoised, cv::Size(3, 3), 0.0);

    cv::Mat enlarged;
    cv::resize(denoised, enlarged, cv::Size(), 2.6, 2.6, cv::INTER_CUBIC);

    cv::Mat normalized;
    cv::normalize(enlarged, normalized, 0, 255, cv::NORM_MINMAX);

    cv::Scalar meanVal;
    cv::Scalar stdDev;
    cv::meanStdDev(normalized, meanVal, stdDev);
    double minStdDev = minStdDev_;
    {
        QMutexLocker locker(&zoneMutex_);
        minStdDev = minStdDev_;
    }
    if (stdDev[0] < minStdDev) {
        return {};
    }

    return normalized.clone();
}
