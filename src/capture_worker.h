#pragma once

#include <QMutex>
#include <QObject>
#include <QRect>

#include <algorithm>
#include <atomic>

#include <opencv2/core.hpp>

#include "tuning_params.h"

Q_DECLARE_METATYPE(cv::Mat)

class CaptureWorker : public QObject
{
    Q_OBJECT

public:
    explicit CaptureWorker(const QRect &scanZone, QObject *parent = nullptr);

public slots:
    void start();
    void stop();
    void setScanZone(const QRect &scanZone);
    void setNoiseParams(double changeThreshold, double minChangedRatio, double minStdDev);

signals:
    void imageProcessed(const cv::Mat &processedImg);

private:
    cv::Mat grabGrayFrame() const;
    cv::Mat preprocessForOcr(const cv::Mat &grayFrame) const;

    QRect scanZone_;
    mutable QMutex zoneMutex_;
    std::atomic_bool running_{false};
    cv::Mat previousSmallGray_;
    double changeThreshold_ = tuning::kChangeThreshold;
    double minChangedRatio_ = tuning::kMinChangedRatio;
    double minStdDev_ = tuning::kMinStdDev;
    const int minIntervalMs_ = 28;
    const int maxIntervalMs_ = 140;
};
