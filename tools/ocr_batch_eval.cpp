#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "ocr_engine.h"
#include "source_language.h"
#include "tuning_params.h"

// Offline OCR evaluation tool (image -> text).
// Runs the same OcrEngine used at runtime over a folder of images and compares the
// recognized text against labels. Translation is intentionally NOT evaluated here:
// runtime translation goes through the local LLM backend, not a bundled model.
//
// Usage: OcrBatchEval [zh|en]
//   zh (default) — reads test/image/image_sub.txt with the Chinese recognition model
//   en           — reads test/image/image_sub_en.txt with the English recognition model
// Both label files use the same "- <imageId>: <expected text>" format.

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

QString normalize(const QString &text, SourceLanguage language)
{
    QString t = text;
    if (language == SourceLanguage::English) {
        // English comparison is case-insensitive and space-normalized rather than
        // space-stripped: word boundaries are part of a correct English recognition,
        // but a single/double space difference is not an error worth counting.
        t = t.simplified().toLower();
        return t.trimmed();
    }

    t.remove(QRegularExpression("\\s+"));
    return t.trimmed();
}

} // namespace

int main(int argc, char *argv[])
{
    const SourceLanguage language =
        (argc > 1) ? sourcelang::fromKey(QString::fromUtf8(argv[1])) : SourceLanguage::Chinese;
    const bool isEnglish = (language == SourceLanguage::English);

    // Evaluate with the same runtime tuning the app uses, so a threshold tuned here is the
    // threshold that actually runs. Edit config/tuning.json and re-run — no rebuild.
    QStringList tuningMessages;
    tuning::loadTuningConfig(QString::fromUtf8(tuning::kTuningConfigPath), &tuningMessages);
    for (const QString &message : tuningMessages) {
        std::cerr << "[tuning] " << message.toStdString() << '\n';
    }
    std::cout << "Tuning config: " << tuning::resolvedTuningConfigPath().toStdString() << '\n';

    const tuning::LanguageProfile &profile = tuning::profileFor(language);
    std::cout << "Model: " << profile.recOnnxPath.toStdString()
              << "\nCharset: " << profile.charsetPath.toStdString()
              << "\ninputWidth=" << profile.inputWidth
              << " minOcrConfidence=" << profile.minOcrConfidence
              << " edgeMinConfidence=" << profile.edgeMinConfidence << "\n\n";

    const std::string imageDir = "../test/image";
    const std::string expectedFile =
        imageDir + (isEnglish ? "/image_sub_en.txt" : "/image_sub.txt");
    const std::string logsDir = "../logs";
    const std::string outputDir = logsDir + "/debug_preprocessed";
    const std::string reportPath =
        logsDir + (isEnglish ? "/ocr_eval_en.txt" : "/ocr_eval.txt");

    std::filesystem::create_directories(logsDir);
    std::filesystem::create_directories(outputDir);

    const auto expected = loadExpected(expectedFile);
    if (expected.empty()) {
        std::cerr << "Failed to load expected labels from " << expectedFile << '\n';
        return 1;
    }

    OcrEngine engine(language);
    if (!engine.isReady()) {
        std::cerr << "OCR engine is not ready for "
                  << sourcelang::displayName(language).toStdString()
                  << " (check ONNX model and charset paths in tuning_params.h)\n";
        return 2;
    }

    int total = 0;
    int ocrExact = 0;
    int ocrExactAndAccepted = 0;
    float lowestCorrectConfidence = 1.0f;
    float highestWrongConfidence = 0.0f;

    std::ofstream report(reportPath, std::ios::trunc);
    report << "# OCR Evaluation (image -> text)\n";
    report << "# language: " << sourcelang::key(language).toStdString() << "\n";
    report << "# labels: " << expectedFile << "\n\n";

    for (const auto &[id, label] : expected) {
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

        const OcrEngine::OcrResult result = engine.performOcr(prepared);
        const QString predRaw = result.text.trimmed();
        const QString pred = normalize(predRaw, language);
        const QString gt = normalize(label, language);

        ++total;
        const bool ocrOk = (!pred.isEmpty() && pred == gt);
        if (ocrOk) {
            ++ocrExact;
        }

        // The confidence is the number minOcrConfidence is compared against at runtime, so
        // it is reported per image: a correct read scoring below the gate would be dropped
        // by the live pipeline even though it shows as a match here.
        const bool wouldPassGate = result.confidence >= profile.minOcrConfidence;
        if (ocrOk && wouldPassGate) {
            ++ocrExactAndAccepted;
        }
        if (ocrOk) {
            lowestCorrectConfidence = std::min(lowestCorrectConfidence, result.confidence);
        } else {
            highestWrongConfidence = std::max(highestWrongConfidence, result.confidence);
        }

        std::cout << id
                  << " | expected=" << gt.toStdString()
                  << " | ocr=" << pred.toStdString()
                  << " | match=" << (ocrOk ? "YES" : "NO")
                  << " | conf=" << result.confidence
                  << (wouldPassGate ? "" : " (BELOW GATE — dropped at runtime)")
                  << " | prepared=" << preparedPath
                  << '\n';

        report << id
               << "\n  expected: " << label.trimmed().toStdString()
               << "\n  ocr: " << predRaw.toStdString()
               << "\n  match: " << (ocrOk ? "YES" : "NO")
               << "\n  confidence: " << result.confidence
               << (wouldPassGate ? "" : "  <-- below minOcrConfidence, dropped at runtime")
               << "\n  prepared: " << preparedPath << "\n\n";
    }

    std::cout << "\nOCR exact match:            " << ocrExact << "/" << total << '\n';
    std::cout << "...and above the gate:      " << ocrExactAndAccepted << "/" << total << '\n';
    if (ocrExact > 0) {
        std::cout << "Lowest confidence, correct: " << lowestCorrectConfidence << '\n';
    }
    if (ocrExact < total) {
        std::cout << "Highest confidence, wrong:  " << highestWrongConfidence << '\n';
    }
    // The usable window for minOcrConfidence: above every wrong read, below every correct
    // one. When it is empty, confidence alone cannot separate them on this image set.
    if (ocrExact > 0 && ocrExact < total) {
        std::cout << "=> set english/chinese minOcrConfidence between "
                  << highestWrongConfidence << " and " << lowestCorrectConfidence
                  << (highestWrongConfidence >= lowestCorrectConfidence
                          ? "  (NO clean split — confidence alone cannot separate these)"
                          : "")
                  << '\n';
    }
    std::cout << "Saved preprocessed images to: " << outputDir << '\n';
    std::cout << "Saved OCR report to: " << reportPath << '\n';

    report << "Summary\n";
    report << "  OCR exact: " << ocrExact << "/" << total << "\n";
    report << "  OCR exact and above minOcrConfidence: " << ocrExactAndAccepted << "/" << total << "\n";
    if (ocrExact > 0) {
        report << "  Lowest confidence among correct reads: " << lowestCorrectConfidence << "\n";
    }
    if (ocrExact < total) {
        report << "  Highest confidence among wrong reads:  " << highestWrongConfidence << "\n";
    }
    return 0;
}
