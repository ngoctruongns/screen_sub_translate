/**
 * Create a C++ class named CaptureWorker that inherits from QObject to run in a separate QThread:
 * - It should take a QRect representing the scanning zone.
 * - In a loop running every 300ms, use QScreen::grabWindow to capture that specific screen area.
 * - Convert the captured QPixmap into an OpenCV cv::Mat (Grayscale).
 * - Apply cv::threshold (Binary Inverse with Otsu) to process the image for Chinese OCR.
 * - To optimize, compare the current frame with the previous frame using cv::absdiff. 
 *   If the mean pixel difference is below a threshold (meaning the subtitle hasn't changed), do not process further.
 * - If the image has changed significantly, emit a Qt signal imageProcessed(const cv::Mat& processedImg)
 */
