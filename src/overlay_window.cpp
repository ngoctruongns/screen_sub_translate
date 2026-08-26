#include "overlay_window.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QDebug>
#include <QMetaObject>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QStringList>

#include "capture_zone_widget.h"
#include "overlay_frame.h"
#include "translation_widget.h"

namespace
{

QString joinSubtitleFragments(const QString &left, const QString &right, SourceLanguage language)
{
    QString merged = left.trimmed();
    const QString next = right.trimmed();

    if (merged.isEmpty()) {
        return next;
    }
    if (next.isEmpty()) {
        return merged;
    }

    const QChar last = merged.back();
    if (last == QChar(0xFF0C) || last == QChar(',') || last == QChar(0x3001)) {
        merged.chop(1);
    }
    merged = merged.trimmed();

    // Han text is written without word spacing, so the two halves butt together; English
    // needs the separator or the merge fuses two words into one.
    if (language == SourceLanguage::English) {
        return merged + QLatin1Char(' ') + next;
    }
    return merged + next;
}

QRect defaultCaptureGeometry()
{
    const QScreen *screen = QGuiApplication::primaryScreen();
    const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    const int w = std::min(900, avail.width() - 40);
    const int h = 120;
    const int x = avail.x() + (avail.width() - w) / 2;
    const int y = avail.y() + static_cast<int>(avail.height() * 0.78);
    return QRect(x, y, w, h);
}

QRect defaultTranslationGeometry()
{
    const QRect cap = defaultCaptureGeometry();
    return QRect(cap.x(), cap.bottom() + 12, cap.width(), 90);
}

} // namespace

OverlayWindow::OverlayWindow(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<cv::Mat>("cv::Mat");
    qRegisterMetaType<SourceLanguage>("SourceLanguage");

    // The backend config supplies the startup language; a language the user picked in a
    // previous session overrides it.
    sourceLanguage_ = restoreSourceLanguage();

    setupWidgets();

    const QString logDir = QDir::cleanPath(
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../logs")));
    subtitleLogger_ = new SubtitleLogger(
        QDir(logDir).filePath(QStringLiteral("subtitle_log.txt")),
        QDir(logDir).filePath(QStringLiteral("subtitles")),
#ifdef SST_DEBUG_BUILD
        true,
#else
        false,
#endif
        sourceLanguage_,
        nullptr);
    subtitleLogger_->moveToThread(&loggerThread_);
    connect(&loggerThread_, &QThread::finished, subtitleLogger_, &QObject::deleteLater);
    loggerThread_.start();
    QMetaObject::invokeMethod(subtitleLogger_, "initialize", Qt::BlockingQueuedConnection);
    appendSubtitleLog(QStringLiteral("SESSION_START"), QString(), QString());

    captureWorker_ = new CaptureWorker(captureZone_->captureRect());
    captureWorker_->moveToThread(&captureThread_);
    // The OCR engine loads its model in the worker's constructor, so the language has to
    // be known before the worker exists rather than pushed in afterwards.
    ocrWorker_ = new OcrWorker(sourceLanguage_);
    ocrWorker_->moveToThread(&ocrThread_);

    connect(&captureThread_, &QThread::started, captureWorker_, &CaptureWorker::start);
    connect(&captureThread_, &QThread::finished, captureWorker_, &QObject::deleteLater);
    connect(&ocrThread_, &QThread::finished, ocrWorker_, &QObject::deleteLater);
    connect(captureWorker_, &CaptureWorker::imageProcessed, this, &OverlayWindow::onImageProcessed);
    connect(ocrWorker_, &OcrWorker::ocrReady, this, &OverlayWindow::onOcrReady);
    connect(ocrWorker_, &OcrWorker::ocrError, this, &OverlayWindow::onOcrError);
    connect(ocrWorker_, &OcrWorker::languageChanged, this, &OverlayWindow::onOcrLanguageChanged);
    connect(&translateClient_, &TranslateClient::translationReady, this, &OverlayWindow::onTranslationReady);
    connect(&translateClient_, &TranslateClient::translationError, this, &OverlayWindow::onTranslationError);

    // Align the rest of the pipeline with the restored language (the OCR worker and the
    // logger already got it through their constructors).
    ocrSubtitleFilter_.setLanguage(sourceLanguage_);
    translateClient_.setSourceLanguage(sourceLanguage_);

    applyDefaultNoiseConfig();

    displayTimer_ = new QTimer(this);
    displayTimer_->setInterval(tuning::kDisplayTickMs);
    connect(displayTimer_, &QTimer::timeout, this, &OverlayWindow::tickDisplayQueue);
    displayTimer_->start();

    captureThread_.start();
    ocrThread_.start();
    updateWorkerScanZone();
}

OverlayWindow::~OverlayWindow()
{
    if (captureWorker_) {
        QMetaObject::invokeMethod(captureWorker_, "stop", Qt::BlockingQueuedConnection);
    }
    captureThread_.quit();
    ocrThread_.quit();
    captureThread_.wait();
    ocrThread_.wait();

    if (subtitleLogger_) {
        QMetaObject::invokeMethod(subtitleLogger_, "shutdown", Qt::BlockingQueuedConnection,
                                  Q_ARG(qint64, QDateTime::currentMSecsSinceEpoch()));
    }
    loggerThread_.quit();
    loggerThread_.wait();

    delete captureZone_;
    delete translation_;
}

void OverlayWindow::setupWidgets()
{
    captureZone_ = new CaptureZoneWidget();
    translation_ = new TranslationWidget();

    restoreWidgetGeometry();

    captureZone_->setSourceLanguage(sourceLanguage_);

    connect(captureZone_, &OverlayFrame::geometryChanged, this, &OverlayWindow::onZoneGeometryChanged);
    connect(translation_, &OverlayFrame::geometryChanged, this, &OverlayWindow::onZoneGeometryChanged);
    connect(captureZone_, &CaptureZoneWidget::sourceLanguageSelected, this,
            &OverlayWindow::setSourceLanguage);

    recomputeBoundingBox();

    captureZone_->show();
    translation_->show();
}

void OverlayWindow::restoreWidgetGeometry()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("overlay"));
    captureZone_->setGeometry(
        settings.value(QStringLiteral("captureZone"), defaultCaptureGeometry()).toRect());
    translation_->setGeometry(
        settings.value(QStringLiteral("translation"), defaultTranslationGeometry()).toRect());
    settings.endGroup();
}

void OverlayWindow::saveWidgetGeometry() const
{
    if (!captureZone_ || !translation_) {
        return;
    }
    QSettings settings;
    settings.beginGroup(QStringLiteral("overlay"));
    settings.setValue(QStringLiteral("captureZone"), captureZone_->geometry());
    settings.setValue(QStringLiteral("translation"), translation_->geometry());
    settings.endGroup();
}

SourceLanguage OverlayWindow::restoreSourceLanguage() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("overlay"));
    const QString stored = settings.value(QStringLiteral("sourceLanguage")).toString();
    settings.endGroup();

    // No stored choice yet: fall back to whatever translation_backend.json asked for.
    if (stored.trimmed().isEmpty()) {
        return translateClient_.configuredSourceLanguage();
    }
    return sourcelang::fromKey(stored, translateClient_.configuredSourceLanguage());
}

void OverlayWindow::saveSourceLanguage() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("overlay"));
    settings.setValue(QStringLiteral("sourceLanguage"), sourcelang::key(sourceLanguage_));
    settings.endGroup();
}

void OverlayWindow::setSourceLanguage(SourceLanguage language)
{
    if (language == sourceLanguage_) {
        return;
    }

    sourceLanguage_ = language;
    saveSourceLanguage();

    if (captureZone_) {
        captureZone_->setSourceLanguage(language);
    }

    // Everything currently in flight belongs to the previous language, so drop it before
    // the new model produces its first read.
    resetPipelineState();

    ocrSubtitleFilter_.setLanguage(language);
    translateClient_.setSourceLanguage(language);

    if (subtitleLogger_) {
        // Queued like every other logger call: it runs on the logger thread and starts a
        // fresh source-side .srt for the new language.
        QMetaObject::invokeMethod(subtitleLogger_, "setSourceLanguage", Qt::QueuedConnection,
                                  Q_ARG(SourceLanguage, language));
    }

    if (ocrWorker_) {
        // Rebuilds the ONNX session on the OCR thread; onOcrLanguageChanged reports the
        // outcome once it completes.
        QMetaObject::invokeMethod(ocrWorker_, "setLanguage", Qt::QueuedConnection,
                                  Q_ARG(SourceLanguage, language));
    }

    appendSubtitleLog(QStringLiteral("SOURCE_LANGUAGE"), sourcelang::key(language), QString());
    qDebug() << "OverlayWindow: source language switched to" << sourcelang::displayName(language);
}

void OverlayWindow::resetPipelineState()
{
    // In-flight OCR results for the previous model must not be matched against the new
    // one: bumping the request id makes any reply that is still in the queue stale.
    ++latestFrameRequestId_;
    inFlightOcrRequestId_ = latestFrameRequestId_;
    ocrBusy_ = false;
    latestFrameForOcr_.release();

    subtitleVisible_ = false;
    lastOcrText_.clear();
    lastTranslation_.clear();
    pendingIncompleteSubtitle_.clear();
    pendingIncompleteSubtitleTimer_.invalidate();
    lastNonEmptySubtitleTimer_.invalidate();

    translationQueue_.clear();
    displayingTranslation_ = false;
    currentDisplayDurationMs_ = 0;

    if (translation_ && !translation_->text().isEmpty()) {
        translation_->clear();
    }
}

void OverlayWindow::onOcrLanguageChanged(SourceLanguage language, bool ok)
{
    if (ok) {
        ocrDisabledMessage_.clear();
        appendSubtitleLog(QStringLiteral("OCR_MODEL_LOADED"), sourcelang::key(language), QString());
        return;
    }

    // The model or its dictionary is missing. OCR is disabled until it is available, so
    // say so in the overlay itself: the alternative is a window that silently never shows
    // a subtitle again, with the reason buried in the console.
    ocrDisabledMessage_ =
        QStringLiteral("No %1 OCR model — check models/paddle (see console for the expected path)")
            .arg(sourcelang::displayName(language));
    qWarning() << "OverlayWindow:" << ocrDisabledMessage_;
    appendSubtitleLog(QStringLiteral("OCR_MODEL_ERROR"), sourcelang::key(language),
                      ocrDisabledMessage_);
    if (translation_) {
        translation_->setText(ocrDisabledMessage_);
    }
}

void OverlayWindow::onZoneGeometryChanged()
{
    updateWorkerScanZone();
    recomputeBoundingBox();
    saveWidgetGeometry();
}

void OverlayWindow::recomputeBoundingBox()
{
    if (!captureZone_ || !translation_) {
        return;
    }
    // The "tool window" is purely the logical bounding box that encloses both
    // independent overlay windows; it is recomputed whenever either one moves.
    const QRect box = captureZone_->geometry().united(translation_->geometry());
    if (box == boundingBox_) {
        return;
    }
    boundingBox_ = box;
    appendSubtitleLog(QStringLiteral("BOUNDING_BOX"),
                      QStringLiteral("%1,%2 %3x%4")
                          .arg(box.x()).arg(box.y()).arg(box.width()).arg(box.height()),
                      QString());
}

void OverlayWindow::onImageProcessed(const cv::Mat &processedImg)
{
    if (processedImg.empty()) {
        return;
    }

    latestFrameForOcr_ = processedImg.clone();
    ++latestFrameRequestId_;

    if (!ocrBusy_) {
        dispatchLatestOcr();
    }
}

void OverlayWindow::onOcrReady(const QString &ocrText, float confidence, int requestId)
{
    if (requestId != inFlightOcrRequestId_) {
        return;
    }

    ocrBusy_ = false;

    if (ocrText.isEmpty()) {

        if (subtitleVisible_ && lastNonEmptySubtitleTimer_.elapsed() >= tuning::kSubtitleDisappearTimeoutMs) {
            flushPendingIncompleteSubtitle();
            subtitleVisible_ = false;
            endSubtitleSegment();
            ocrSubtitleFilter_.onSubtitleDisappeared();

            qDebug() << "Subtitle disappeared due to timeout after empty OCR result.";
            tickDisplayQueue(); // Clears display immediately if queue is also empty
        }
    } else {
        // OCR result is non-empty
        // appendSubtitleLog(QStringLiteral("OCR_CAP-->"), ocrText, QString());
        lastNonEmptySubtitleTimer_.restart();

        // Drop garbled low-confidence reads before they reach the filter/translation.
        if (confidence < tuning::profileFor(sourceLanguage_).minOcrConfidence) {
            // qDebug() << "OCR_LOW_CONFIDENCE: text=" << ocrText << "confidence=" << confidence;
            if (latestFrameRequestId_ > requestId) {
                dispatchLatestOcr();
            }
            return;
        }

        const OcrSubtitleFilter::Decision decision = ocrSubtitleFilter_.process(ocrText);

        if (decision.rejectedForQuality) {
            // appendSubtitleLog(QStringLiteral("OCR_REJECTED"), ocrText,
            //                   QStringLiteral("reason=han_quality"));

            if (latestFrameRequestId_ > requestId) {
                dispatchLatestOcr();
            }
            return;
        }

        if (decision.rolledBack) {
            appendSubtitleLog(QStringLiteral("OCR_ROLLBACK"), ocrText,
                              QStringLiteral("best=") + decision.rollbackBestText);
        }

        if (decision.shouldDispatch) {
            handleDispatchCandidate(decision, confidence);
        }
    }

    if (latestFrameRequestId_ > requestId) {
        dispatchLatestOcr();
    }
}

void OverlayWindow::handleDispatchCandidate(const OcrSubtitleFilter::Decision &decision, float confidence)
{
    QString dispatchText = decision.dispatchText.trimmed();
    if (dispatchText.isEmpty()) {
        return;
    }

    if (!pendingIncompleteSubtitle_.isEmpty()) {
        dispatchText = joinSubtitleFragments(pendingIncompleteSubtitle_, dispatchText, sourceLanguage_);
        appendSubtitleLog(QStringLiteral("OCR_MERGED"), dispatchText,
                          QStringLiteral("prefix=") + pendingIncompleteSubtitle_);
        pendingIncompleteSubtitle_.clear();
        pendingIncompleteSubtitleTimer_.invalidate();
    }

    if (isIncompleteSubtitlePhrase(dispatchText, sourceLanguage_) &&
        decision.seenFrames >= tuning::kMinIncompleteHoldFrames) {
        pendingIncompleteSubtitle_ = dispatchText;
        pendingIncompleteSubtitleTimer_.restart();
        appendSubtitleLog(QStringLiteral("OCR_HELD_INCOMPLETE"), dispatchText,
                          QString("stable=%1ms frames=%2").arg(decision.stableElapsedMs).arg(decision.seenFrames));
        qDebug() << "OCR_HELD_INCOMPLETE: text=" << dispatchText
                 << "stableElapsedMs=" << decision.stableElapsedMs
                 << "seenFrames=" << decision.seenFrames;
        return;
    }

    subtitleVisible_ = true;
    appendSubtitleLog(QStringLiteral("OCR_DETECTED"), dispatchText,
                      QString("stable=%1ms frames=%2").arg(decision.stableElapsedMs).arg(decision.seenFrames));

    startSubtitleSegment(dispatchText);
    translateClient_.requestTranslation(dispatchText);
    lastOcrText_ = dispatchText;

    qDebug() << "OCR_DETECTED: text=" << dispatchText
             << "stableElapsedMs=" << decision.stableElapsedMs
             << "seenFrames=" << decision.seenFrames
             << "confidence=" << confidence;
}

void OverlayWindow::flushPendingIncompleteSubtitle()
{
    if (pendingIncompleteSubtitle_.isEmpty()) {
        return;
    }

    QString text = pendingIncompleteSubtitle_.trimmed();
    pendingIncompleteSubtitle_.clear();
    pendingIncompleteSubtitleTimer_.invalidate();

    // The trailing pause separator only marked the line as "maybe continued"; strip it so
    // the backend sees a finished clause.
    while (!text.isEmpty() &&
           (text.endsWith(QChar(0xFF0C)) || text.endsWith(QChar(',')) || text.endsWith(QChar(0x3001)))) {
        text.chop(1);
        text = text.trimmed();
    }

    if (text.isEmpty()) {
        return;
    }

    // No continuation ever arrived (scene cut, or the trailing comma was spurious OCR).
    // Translate the held fragment instead of dropping it — a clause that merely ended with
    // a comma is still worth showing.
    appendSubtitleLog(QStringLiteral("OCR_FLUSHED_INCOMPLETE"), text, QString());
    qDebug() << "OCR_FLUSHED_INCOMPLETE: text=" << text;
    translateClient_.requestTranslation(text);
    lastOcrText_ = text;
}

bool OverlayWindow::isIncompleteSubtitlePhrase(const QString &text, SourceLanguage language)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }

    // A trailing comma only signals "maybe continued" for a SHORT lead-in clause. A long
    // clause ending in a comma already carries a full unit, so translate it now instead of
    // waiting; this also avoids holding a long line whose trailing comma is an OCR margin
    // hallucination. This part is shared: only the unit of measure differs per language.
    if (trimmed.endsWith(QChar(0xFF0C)) || trimmed.endsWith(QChar(',')) || trimmed.endsWith(QChar(0x3001))) {
        return OcrSubtitleFilter::contentUnitCount(trimmed, language) <=
               tuning::profileFor(language).maxIncompleteHoldUnits;
    }

    // What counts as "cut mid-construction" is entirely language-specific, so each
    // language brings its own test.
    return language == SourceLanguage::English ? isIncompleteEnglishPhrase(trimmed)
                                               : isIncompleteChinesePhrase(trimmed);
}

bool OverlayWindow::isIncompleteChinesePhrase(const QString &trimmed)
{
    // Grammatical mid-phrase suffixes are held regardless of length: the clause is cut
    // mid-construction and cannot be translated correctly on its own.
    static const QStringList kIncompleteSuffixes = {
        QStringLiteral("当作一"),
        QStringLiteral("作为一"),
        QStringLiteral("成为一"),
        QStringLiteral("看作一"),
        QStringLiteral("视为一")
    };

    for (const QString &suffix : kIncompleteSuffixes) {
        if (trimmed.endsWith(suffix)) {
            return true;
        }
    }

    return false;
}

bool OverlayWindow::isIncompleteEnglishPhrase(const QString &trimmed)
{
    // An English subtitle split across two cards almost always breaks after a function
    // word: a conjunction, a preposition, an article or an auxiliary. Translating such a
    // fragment alone produces a dangling clause, so it is held for its continuation.
    // Content words are never held — a card ending in a noun or a verb is a complete unit.
    static const QSet<QString> kTrailingFunctionWords = {
        QStringLiteral("and"),   QStringLiteral("but"),   QStringLiteral("or"),
        QStringLiteral("nor"),   QStringLiteral("so"),    QStringLiteral("yet"),
        QStringLiteral("that"),  QStringLiteral("which"), QStringLiteral("who"),
        QStringLiteral("whom"),  QStringLiteral("whose"), QStringLiteral("because"),
        QStringLiteral("since"), QStringLiteral("while"), QStringLiteral("when"),
        QStringLiteral("where"), QStringLiteral("if"),    QStringLiteral("unless"),
        QStringLiteral("until"), QStringLiteral("than"),  QStringLiteral("as"),
        QStringLiteral("to"),    QStringLiteral("of"),    QStringLiteral("in"),
        QStringLiteral("on"),    QStringLiteral("at"),    QStringLiteral("by"),
        QStringLiteral("for"),   QStringLiteral("with"),  QStringLiteral("from"),
        QStringLiteral("into"),  QStringLiteral("onto"),  QStringLiteral("about"),
        QStringLiteral("the"),   QStringLiteral("a"),     QStringLiteral("an"),
        QStringLiteral("is"),    QStringLiteral("are"),   QStringLiteral("was"),
        QStringLiteral("were"),  QStringLiteral("been"),  QStringLiteral("be"),
        QStringLiteral("have"),  QStringLiteral("has"),   QStringLiteral("had"),
        QStringLiteral("will"),  QStringLiteral("would"), QStringLiteral("can"),
        QStringLiteral("could"), QStringLiteral("should"),
    };

    // A line closed by terminal punctuation is finished no matter what word precedes it.
    const QChar last = trimmed.back();
    if (last == QChar('.') || last == QChar('!') || last == QChar('?') || last == QChar(':')) {
        return false;
    }

    const int lastSpace = trimmed.lastIndexOf(QChar(' '));
    if (lastSpace < 0) {
        return false; // A single word is never held: there is nothing to merge it into.
    }

    QString lastWord = trimmed.mid(lastSpace + 1).toLower();
    while (!lastWord.isEmpty() && !lastWord.back().isLetter()) {
        lastWord.chop(1);
    }

    return kTrailingFunctionWords.contains(lastWord);
}

void OverlayWindow::onOcrError(const QString &error, int requestId)
{
    if (requestId != inFlightOcrRequestId_) {
        return;
    }

    ocrBusy_ = false;
    qWarning() << "[OCR_ERROR]" << error;
    appendSubtitleLog(QStringLiteral("OCR_ERROR"), QString::number(requestId), error);
    if (subtitleVisible_ && lastNonEmptySubtitleTimer_.elapsed() >= tuning::kSubtitleDisappearTimeoutMs) {
        subtitleVisible_ = false;
        endSubtitleSegment();
        ocrSubtitleFilter_.onSubtitleDisappeared();
        qDebug() << "OCR_ERROR: Subtitle disappeared due to timeout after error.";
        tickDisplayQueue(); // Clears display immediately if queue is also empty
    }

    if (latestFrameRequestId_ > requestId) {
        dispatchLatestOcr();
    }
}

void OverlayWindow::onTranslationReady(const QString &translatedText, const QString &sourceText)
{
    if (translatedText.isEmpty() || translatedText == lastTranslation_) {
        return;
    }

    appendSubtitleLog(QStringLiteral("TRANSLATED"), sourceText, translatedText);
    updateSubtitleSegmentTranslation(sourceText, translatedText);
    enqueueTranslation(translatedText, sourceText);
}

void OverlayWindow::onTranslationError(const QString &error)
{
    // Keep the last subtitle on screen to avoid flicker during transient network failures.
    appendSubtitleLog(QStringLiteral("TRANSLATE_ERROR"), lastOcrText_, error);
}

void OverlayWindow::updateWorkerScanZone()
{
    if (!captureWorker_ || !captureZone_) {
        return;
    }

    const QRect newZone = captureZone_->captureRect();
    if (newZone == lastSentScanZone_) {
        return;
    }
    lastSentScanZone_ = newZone;
    QMetaObject::invokeMethod(captureWorker_, "setScanZone", Qt::QueuedConnection,
                              Q_ARG(QRect, newZone));
}

void OverlayWindow::appendSubtitleLog(const QString &status, const QString &sourceText,
                                      const QString &translatedText) const
{
    if (!subtitleLogger_) {
        return;
    }

    QMetaObject::invokeMethod(subtitleLogger_, "logDebugEvent", Qt::QueuedConnection,
                              Q_ARG(QString, status), Q_ARG(QString, sourceText),
                              Q_ARG(QString, translatedText));
}

void OverlayWindow::startSubtitleSegment(const QString &sourceText) const
{
    if (!subtitleLogger_ || sourceText.isEmpty()) {
        return;
    }

    QMetaObject::invokeMethod(subtitleLogger_, "startSubtitle", Qt::QueuedConnection,
                              Q_ARG(QString, sourceText),
                              Q_ARG(qint64, QDateTime::currentMSecsSinceEpoch()));
}

void OverlayWindow::updateSubtitleSegmentTranslation(const QString &sourceText,
                                                     const QString &translatedText) const
{
    if (!subtitleLogger_ || sourceText.isEmpty() || translatedText.isEmpty()) {
        return;
    }

    QMetaObject::invokeMethod(subtitleLogger_, "updateTranslation", Qt::QueuedConnection,
                              Q_ARG(QString, sourceText), Q_ARG(QString, translatedText));
}

void OverlayWindow::endSubtitleSegment() const
{
    if (!subtitleLogger_) {
        return;
    }

    QMetaObject::invokeMethod(subtitleLogger_, "endSubtitle", Qt::QueuedConnection,
                              Q_ARG(qint64, QDateTime::currentMSecsSinceEpoch()));
}

void OverlayWindow::dispatchLatestOcr()
{
    if (!ocrWorker_ || latestFrameForOcr_.empty()) {
        return;
    }

    ocrBusy_ = true;
    inFlightOcrRequestId_ = latestFrameRequestId_;

    QMetaObject::invokeMethod(ocrWorker_, "processImage", Qt::QueuedConnection,
                              Q_ARG(cv::Mat, latestFrameForOcr_),
                              Q_ARG(int, inFlightOcrRequestId_));
}

void OverlayWindow::applyDefaultNoiseConfig()
{
    minOcrLength_ = tuning::kMinOcrLength;
    minCandidateStableMs_ = tuning::kMinCandidateStableMs;
    ocrSubtitleFilter_.configure(minOcrLength_, minCandidateStableMs_);

    if (captureWorker_) {
        QMetaObject::invokeMethod(captureWorker_, "setNoiseParams", Qt::QueuedConnection,
                                  Q_ARG(double, tuning::kChangeThreshold),
                                  Q_ARG(double, tuning::kMinChangedRatio),
                                  Q_ARG(double, tuning::kMinStdDev));
    }
}

void OverlayWindow::enqueueTranslation(const QString &translatedText, const QString &sourceText)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Drop stale entries from the front to reclaim queue space before inserting.
    while (!translationQueue_.isEmpty() &&
           now - translationQueue_.head().enqueuedAtMs > tuning::kDisplayMaxLatencyMs) {
        const TranslationEntry dropped = translationQueue_.dequeue();
        appendSubtitleLog(QStringLiteral("DISPLAY_DROPPED_STALE"), dropped.sourceText,
                          dropped.translatedText);
    }

    // immediately on the next tick, preventing unbounded latency build-up.
    if (translationQueue_.size() >= tuning::kDisplayQueueMaxSize) {
        qDebug() << "Translation queue overflow: dropping oldest entry to make space.";
        translationQueue_.dequeue(); // Remove the oldest entry to make space
    }

    TranslationEntry entry;
    entry.translatedText = translatedText;
    entry.sourceText     = sourceText;
    entry.enqueuedAtMs   = now;
    translationQueue_.enqueue(entry);

    // appendSubtitleLog(QStringLiteral("DISPLAY_ENQUEUED"), sourceText,
    //                   QString("queue_depth=%1").arg(translationQueue_.size()));
}

void OverlayWindow::tickDisplayQueue()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Purge entries at the front of the queue that are too old to be relevant.
    while (!translationQueue_.isEmpty() &&
           now - translationQueue_.head().enqueuedAtMs > tuning::kDisplayMaxLatencyMs) {
        const TranslationEntry dropped = translationQueue_.dequeue();
        appendSubtitleLog(QStringLiteral("DISPLAY_DROPPED_STALE"), dropped.sourceText,
                          dropped.translatedText);
    }

    if (displayingTranslation_) {
        if (currentDisplayTimer_.elapsed() < currentDisplayDurationMs_) {
            return; // Still within the allocated display window for the current entry
        }
        displayingTranslation_ = false;
    }

    if (!translationQueue_.isEmpty()) {
        showTranslationEntry(translationQueue_.dequeue());
    } else if (!subtitleVisible_) {
        // Queue is drained and the source subtitle has disappeared. Normally that means
        // clearing the overlay — unless OCR is down for a missing model, in which case the
        // reason stays on screen rather than being wiped by the next tick.
        if (!ocrDisabledMessage_.isEmpty()) {
            if (translation_ && translation_->text() != ocrDisabledMessage_) {
                translation_->setText(ocrDisabledMessage_);
            }
        } else if (translation_ && !translation_->text().isEmpty()) {
            translation_->clear();
            appendSubtitleLog(QStringLiteral("DISPLAY_CLEARED"), QString(), QString());
        }
    }
}

void OverlayWindow::showTranslationEntry(const TranslationEntry &entry)
{
    lastTranslation_          = entry.translatedText;
    // Base reading time on the displayed Vietnamese text, not the source line: a Han line is
    // much shorter than its Vietnamese rendering, which otherwise clears subtitles too early.
    // Measuring the output keeps this correct for either source language.
    currentDisplayDurationMs_ = computeDisplayDurationMs(entry.translatedText);
    currentDisplayTimer_.restart();
    displayingTranslation_    = true;

    if (translation_) {
        translation_->setText(entry.translatedText);
    }

    // appendSubtitleLog(QStringLiteral("DISPLAY_SHOW"), entry.sourceText,
    //                   entry.translatedText + QString(" dur=%1ms").arg(currentDisplayDurationMs_));
}

int OverlayWindow::computeDisplayDurationMs(const QString &text) const
{
    // Base duration + per-character contribution, clamped to configured bounds.
    const int charCount = text.size();
    const int duration  = tuning::kDisplayBaseMs + charCount * tuning::kDisplayMsPerChar;
    return std::clamp(duration, tuning::kDisplayMinMs, tuning::kDisplayMaxMs);
}
