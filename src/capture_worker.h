#pragma once

#include <QMutex>
#include <QObject>
#include <QRect>

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
    struct WorkerParams
    {
        QRect scanZone;
        double changeThreshold = tuning::kChangeThreshold;
        double minChangedRatio = tuning::kMinChangedRatio;
        double minStdDev = tuning::kMinStdDev;
    };

    WorkerParams paramsSnapshot() const;
    cv::Mat grabGrayFrame(const QRect &zone) const;
    cv::Mat preprocessForOcr(const cv::Mat &grayFrame, double minStdDev) const;

    mutable QMutex paramsMutex_;
    WorkerParams params_;
    std::atomic_bool running_{false};
    cv::Mat previousSmallGray_;
};
