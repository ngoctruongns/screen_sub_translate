/**
 * Write a C++ class named OcrEngine using the Tesseract C++ API (tesseract::TessBaseAPI):
 * - In the constructor, initialize Tesseract with the Chinese Simplified language model (chi_sim).
 * - Implement a method QString performOcr(const cv::Mat& inputImg) that takes the preprocessed OpenCV matrix from the CaptureWorker, 
 *   feeds it into Tesseract, extracts the Chinese text, removes all spaces from the string, and returns the cleaned text.
 * - Handle clean up properly in the destructor by calling End().
 */