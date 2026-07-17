#include <QRegularExpression>
#include <QString>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "ocr_engine.h"

// Offline OCR evaluation tool (image -> text).
// Runs the same OcrEngine used at runtime over a folder of images and compares the
// recognized Chinese text against labels. Translation is intentionally NOT evaluated
// here: runtime translation goes through the local LLM backend, not a bundled model.

namespace {

std::map<std::string, QString> loadExpected(const std::string &filePath)
{
    std::ifstream in(filePath);
    std::map<std::string, QString> expected;
    std::string line;

    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }

        const std::size_t dashPos = line.find("- ");
        const std::size_t colonPos = line.find(':');
        if (dashPos == std::string::npos || colonPos == std::string::npos || colonPos <= dashPos + 2) {
            continue;
        }

        std::string key = line.substr(dashPos + 2, colonPos - (dashPos + 2));
        while (!key.empty() && key.back() == ' ') {
            key.pop_back();
        }

        std::string value = line.substr(colonPos + 1);
        while (!value.empty() && value.front() == ' ') {
            value.erase(value.begin());
        }

        expected[key] = QString::fromUtf8(value.c_str()).trimmed();
    }

    return expected;
}

cv::Mat prepareForOcr(const cv::Mat &bgr)
{
    if (bgr.empty()) {
        return {};
    }

    cv::Mat gray;
    if (bgr.channels() == 1) {
        gray = bgr.clone();
    } else {
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    }

    cv::Mat denoised;
    cv::GaussianBlur(gray, denoised, cv::Size(3, 3), 0.0);

    cv::Mat enlarged;
    cv::resize(denoised, enlarged, cv::Size(), 2.6, 2.6, cv::INTER_CUBIC);

    return enlarged;
}

QString normalize(const QString &text)
{
    QString t = text;
    t.remove(QRegularExpression("\\s+"));
    return t.trimmed();
}

} // namespace

int main()
{
    const std::string imageDir = "../test/image";
    const std::string expectedFileZh = imageDir + "/image_sub.txt";
    const std::string logsDir = "../logs";
    const std::string outputDir = logsDir + "/debug_preprocessed";
    const std::string reportPath = logsDir + "/ocr_eval.txt";

    std::filesystem::create_directories(logsDir);
    std::filesystem::create_directories(outputDir);

    const auto expectedZh = loadExpected(expectedFileZh);
    if (expectedZh.empty()) {
        std::cerr << "Failed to load expected labels from " << expectedFileZh << '\n';
        return 1;
    }

    OcrEngine engine;
    if (!engine.isReady()) {
        std::cerr << "OCR engine is not ready (check ONNX model and charset paths)\n";
        return 2;
    }

    int total = 0;
    int ocrExact = 0;

    std::ofstream report(reportPath, std::ios::trunc);
    report << "# OCR Evaluation (image -> text)\n";
    report << "# zh labels: " << expectedFileZh << "\n\n";

    for (const auto &[id, label] : expectedZh) {
        const std::string imagePath = imageDir + "/" + id + ".png";
        const cv::Mat img = cv::imread(imagePath, cv::IMREAD_COLOR);
        if (img.empty()) {
            std::cerr << "Failed to read image: " << imagePath << '\n';
            continue;
        }

        const cv::Mat prepared = prepareForOcr(img);
        const std::string preparedPath = outputDir + "/" + id + "_prepared.png";
        if (!prepared.empty()) {
            cv::imwrite(preparedPath, prepared);
        }

        const QString predRaw = engine.performOcr(prepared).text.trimmed();
        const QString pred = normalize(predRaw);
        const QString gt = normalize(label);

        ++total;
        const bool ocrOk = (!pred.isEmpty() && pred == gt);
        if (ocrOk) {
            ++ocrExact;
        }

        std::cout << id
                  << " | expected=" << gt.toStdString()
                  << " | ocr=" << pred.toStdString()
                  << " | match=" << (ocrOk ? "YES" : "NO")
                  << " | prepared=" << preparedPath
                  << '\n';

        report << id
               << "\n  expected: " << label.trimmed().toStdString()
               << "\n  ocr: " << predRaw.toStdString()
               << "\n  match: " << (ocrOk ? "YES" : "NO")
               << "\n  prepared: " << preparedPath << "\n\n";
    }

    std::cout << "OCR exact match: " << ocrExact << "/" << total << '\n';
    std::cout << "Saved preprocessed images to: " << outputDir << '\n';
    std::cout << "Saved OCR report to: " << reportPath << '\n';

    report << "Summary\n";
    report << "  OCR exact: " << ocrExact << "/" << total << "\n";
    return 0;
}
