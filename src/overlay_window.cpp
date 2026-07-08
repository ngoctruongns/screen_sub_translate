#include "overlay_window.h"

#include <algorithm>
#include <cstdlib>

#include <QAction>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFontMetrics>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QShortcut>
#include <QTextStream>

namespace
{
int countHanChars(const QString &text)
{
    int count = 0;
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        const bool isHan =
            (u >= 0x3400 && u <= 0x4DBF) || (u >= 0x4E00 && u <= 0x9FFF) || (u >= 0xF900 && u <= 0xFAFF);
        if (isHan) {
            ++count;
        }
    }
    return count;
}

int requiredStableMsForCandidate(const QString &candidate, int baseStableMs)
{
    if (candidate.size() <= 1) {
        return std::max(baseStableMs, tuning::kVeryShortCandidateStableMs);
    }

    if (candidate.size() <= 3) {
        return std::max(baseStableMs, tuning::kShortCandidateStableMs);
    }

    return baseStableMs;
}

int requiredSeenFramesForCandidate(const QString &candidate)
{
    if (candidate.size() <= 1) {
        return tuning::kVeryShortCandidateMinFrames;
    }

    if (candidate.size() <= 3) {
        return tuning::kShortCandidateMinFrames;
    }

    return 1;
}

int candidateQualityScore(const QString &text)
{
    // Prefer candidates with more Han chars and then longer length.
    // This avoids locking onto overly short partial OCR fragments.
    return countHanChars(text) * 4 + text.size();
}
} // namespace

OverlayWindow::OverlayWindow(QWidget *parent) : QWidget(parent)
{
    qRegisterMetaType<cv::Mat>("cv::Mat");

    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
    setMinimumSize(500, 120);
    resize(1200, 200);

    setupUi();
    setupHotkeys();
    move(400, 850);

    const QString testDir = QDir::cleanPath(
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../test")));
    subtitleLogger_ = new SubtitleLogger(
        QDir(testDir).filePath(QStringLiteral("subtitle_log.txt")),
        QDir(testDir).filePath(QStringLiteral("subtitles")),
#ifdef SST_DEBUG_BUILD
        true,
#else
        false,
#endif
        nullptr);
    subtitleLogger_->moveToThread(&loggerThread_);
    connect(&loggerThread_, &QThread::finished, subtitleLogger_, &QObject::deleteLater);
    loggerThread_.start();
    QMetaObject::invokeMethod(subtitleLogger_, "initialize", Qt::BlockingQueuedConnection);
    appendSubtitleLog(QStringLiteral("SESSION_START"), QString(), QString());

    captureWorker_ = new CaptureWorker(computeCaptureZone());
    captureWorker_->moveToThread(&captureThread_);
    ocrWorker_ = new OcrWorker();
    ocrWorker_->moveToThread(&ocrThread_);

    connect(&captureThread_, &QThread::started, captureWorker_, &CaptureWorker::start);
    connect(&captureThread_, &QThread::finished, captureWorker_, &QObject::deleteLater);
    connect(&ocrThread_, &QThread::finished, ocrWorker_, &QObject::deleteLater);
    connect(captureWorker_, &CaptureWorker::imageProcessed, this, &OverlayWindow::onImageProcessed);
    connect(ocrWorker_, &OcrWorker::ocrReady, this, &OverlayWindow::onOcrReady);
    connect(ocrWorker_, &OcrWorker::ocrError, this, &OverlayWindow::onOcrError);
    connect(&translateClient_, &TranslateClient::translationReady, this, &OverlayWindow::onTranslationReady);
    connect(&translateClient_, &TranslateClient::translationError, this, &OverlayWindow::onTranslationError);

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
}

void OverlayWindow::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(100, 100, 0, 50), 2, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 8, 8);

    const QRect scanZone = localScanZoneRect();
    painter.setPen(QPen(QColor(255, 180, 0, 50), 1, Qt::DashLine));
    painter.drawRect(scanZone);
}

void OverlayWindow::moveEvent(QMoveEvent *event)
{
    QWidget::moveEvent(event);
    updateWorkerScanZone();
}

void OverlayWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateSubtitleLayout();
    updateWorkerScanZone();
}

void OverlayWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) {
        showPositionMenu(event->globalPosition().toPoint());
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        activeResizeEdges_ = hitTestEdges(event->position().toPoint());
        initialGeometry_ = geometry();
        initialMouseGlobalPos_ = event->globalPosition().toPoint();

        if (activeResizeEdges_ != Qt::Edges()) {
            resizing_ = true;
        } else {
            dragging_ = true;
            dragOffset_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
        }
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void OverlayWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (resizing_ && (event->buttons() & Qt::LeftButton)) {
        applyResize(event->globalPosition().toPoint());
        event->accept();
        return;
    }

    if (dragging_ && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - dragOffset_);
        event->accept();
        return;
    }

    updateCursorForPosition(event->position().toPoint());
    QWidget::mouseMoveEvent(event);
}

void OverlayWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        dragging_ = false;
        resizing_ = false;
        activeResizeEdges_ = Qt::Edges();
        unsetCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
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

void OverlayWindow::onOcrReady(const QString &ocrText, int requestId)
{
    if (requestId != inFlightOcrRequestId_) {
        return;
    }

    ocrBusy_ = false;

    if (ocrText.isEmpty()) {

        if (subtitleVisible_ && lastNonEmptySubtitleTimer_.elapsed() >= tuning::kSubtitleDisappearTimeoutMs) {
            subtitleVisible_ = false;
            endSubtitleSegment();
            recentSubtitleKeys_.clear();
            candidateText_.clear();
            candidateTimer_.invalidate();
            candidateSeenFrames_ = 0;
            candidateFrequency_.clear();

            qDebug() << "Subtitle disappeared due to timeout after empty OCR result.";
            tickDisplayQueue(); // Clears display immediately if queue is also empty
        }
    } else {
        // OCR result is non-empty
        // appendSubtitleLog(QStringLiteral("OCR_CAP-->"), ocrText, QString());
        lastNonEmptySubtitleTimer_.restart();

        if (countHanChars(ocrText) < tuning::kMinHanCharsForCandidate) {
            candidateText_.clear();
            candidateTimer_.invalidate();
            candidateSeenFrames_ = 0;
            candidateFrequency_.clear();
            // appendSubtitleLog(QStringLiteral("OCR_REJECTED"), ocrText,
            //                   QStringLiteral("reason=han_quality"));

            if (latestFrameRequestId_ > requestId) {
                dispatchLatestOcr();
            }
            return;
        }

        if (!isLikelySameSubtitle(ocrText, candidateText_)) {
            // Derive most frequent string seen so far in the current candidate group
            const QString bestText = mostFrequentCandidate();

            // Substring relation can indicate partial/corrupted OCR reads, but we avoid collapsing
            // richer text into a single-character fragment.
            const int minLen = std::min(ocrText.size(), bestText.size());
            const int maxLen = std::max(ocrText.size(), bestText.size());
            const bool hasContainment = !bestText.isEmpty() &&
                                        (ocrText.contains(bestText) || bestText.contains(ocrText));
            const bool isPartialRead = hasContainment && minLen >= 2 && (maxLen - minLen) <= 2;
            if (isPartialRead) {
                // Keep the higher-quality candidate to recover from short noisy fragments.
                candidateFrequency_[ocrText]++;
                candidateText_ = (candidateQualityScore(ocrText) >= candidateQualityScore(bestText))
                                     ? ocrText
                                     : bestText;
                appendSubtitleLog(QStringLiteral("OCR_ROLLBACK"), ocrText,
                                  QStringLiteral("best=") + candidateText_);
            } else {
                // No substring relation — treat as a genuinely new subtitle
                candidateFrequency_.clear();
                candidateFrequency_[ocrText] = 1;
                candidateText_ = ocrText;
                candidateTimer_.restart();
                candidateSeenFrames_ = 1;
            }
        } else {
            // Same subtitle group — accumulate frequency for each OCR variant
            if (!candidateTimer_.isValid()) {
                candidateTimer_.restart();
            }
            candidateFrequency_[ocrText]++;
            ++candidateSeenFrames_;
            // Pick the most frequently seen variant as the representative candidate
            candidateText_ = mostFrequentCandidate();
        }

        const bool enoughLength = candidateText_.size() >= minOcrLength_;
        const int requiredStableMs = requiredStableMsForCandidate(candidateText_, minCandidateStableMs_);
        const int requiredFrames = requiredSeenFramesForCandidate(candidateText_);
        const bool stableEnough =
            candidateTimer_.isValid() && candidateTimer_.elapsed() >= requiredStableMs;
        const bool seenEnoughFrames = candidateSeenFrames_ >= requiredFrames;

        if (enoughLength && stableEnough && seenEnoughFrames &&
            shouldDispatchSubtitle(candidateText_)) {
            appendSubtitleLog(QStringLiteral("OCR_DETECTED"), candidateText_,
                              QString("stable=%1ms frames=%2").arg(candidateTimer_.elapsed()).arg(candidateSeenFrames_));

            startSubtitleSegment(candidateText_);
            translateClient_.requestTranslation(candidateText_);
            lastOcrText_ = candidateText_;
        }
    }

    if (latestFrameRequestId_ > requestId) {
        dispatchLatestOcr();
    }
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

        recentSubtitleKeys_.clear();
        candidateText_.clear();
        candidateTimer_.invalidate();
        candidateSeenFrames_ = 0;
        candidateFrequency_.clear();
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

void OverlayWindow::setupUi()
{
    subtitleLabel_ = new QLabel(
        QStringLiteral("Right-click: options | Alt+T Toggle position"),
        this);
    subtitleLabel_->setAlignment(Qt::AlignCenter);
    subtitleLabel_->setWordWrap(true);
    subtitleLabel_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    subtitleLabel_->setStyleSheet("QLabel {"
                                  "background-color: rgba(0, 0, 0, 165);"
                                  "color: rgb(0, 255, 100);"
                                  "font-size: 30px;"
                                  "font-weight: 700;"
                                  "border-radius: 10px;"
                                  "padding: 12px 16px;"
                                  "}");
    updateSubtitleLayout();
}

void OverlayWindow::setupHotkeys()
{
    auto *positionToggleShortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+T")), this);

    connect(positionToggleShortcut, &QShortcut::activated, this, [this]() {
        resultPosition_ = (resultPosition_ == ResultPosition::AboveSource)
                              ? ResultPosition::BelowSource
                              : ResultPosition::AboveSource;
        updateSubtitleLayout();
        updateWorkerScanZone();
        appendSubtitleLog(QStringLiteral("POSITION_TOGGLED"), QStringLiteral("hotkey"),
                          resultPosition_ == ResultPosition::AboveSource ? QStringLiteral("Above")
                                                                         : QStringLiteral("Below"));
    });
}

void OverlayWindow::updateWorkerScanZone()
{
    if (!captureWorker_) {
        return;
    }

    const QRect newZone = computeCaptureZone();
    if (newZone == lastSentScanZone_) {
        return;
    }
    lastSentScanZone_ = newZone;
    QMetaObject::invokeMethod(captureWorker_, "setScanZone", Qt::QueuedConnection,
                              Q_ARG(QRect, newZone));
    update();
}

void OverlayWindow::updateSubtitleLayout()
{
    if (!subtitleLabel_) {
        return;
    }

    const int outerMargin = 12;
    const int bubblePaddingH = 32;
    const int bubblePaddingV = 20;
    const int maxWidth = std::max(180, width() - outerMargin * 2 - 8);
    const int maxHeight = std::max(46, static_cast<int>(height() * 0.42));

    const QString text =
        subtitleLabel_->text().isEmpty() ? QStringLiteral("...") : subtitleLabel_->text();

    const QFontMetrics fm(subtitleLabel_->font());
    const int candidateWidth =
        std::max(120, static_cast<int>(text.size()) * std::max(10, fm.averageCharWidth()));
    const int bubbleWidth = std::clamp(candidateWidth + bubblePaddingH, 180, maxWidth);

    const QRect textRect = fm.boundingRect(QRect(0, 0, bubbleWidth - bubblePaddingH, 2000),
                                           Qt::TextWordWrap | Qt::AlignCenter, text);
    const int bubbleHeight = std::clamp(textRect.height() + bubblePaddingV, 46, maxHeight);

    const QRect scanRect = localScanZoneRect();
    const int x = (width() - bubbleWidth) / 2;
    int y = outerMargin;

    if (resultPosition_ == ResultPosition::AboveSource) {
        y = std::max(outerMargin, scanRect.top() - bubbleHeight - 8);
    } else {
        y = std::min(height() - bubbleHeight - outerMargin, scanRect.bottom() + 8);
    }

    subtitleLabel_->setGeometry(x, y, bubbleWidth, bubbleHeight);
}

QRect OverlayWindow::localScanZoneRect() const
{
    const int margin = 8;
    const int gap = 8;
    const int labelH = subtitleLabel_ ? subtitleLabel_->height() : 52;
    const int left = margin;
    const int widthLocal = std::max(50, width() - margin * 2);

    int top = margin;
    int heightLocal = height() - margin * 2;

    if (resultPosition_ == ResultPosition::AboveSource) {
        top = margin + labelH + gap;
        heightLocal = height() - top - margin;
    } else {
        top = margin;
        heightLocal = height() - margin * 2 - labelH - gap;
    }

    heightLocal = std::max(40, heightLocal);
    return QRect(left, top, widthLocal, heightLocal);
}

void OverlayWindow::showPositionMenu(const QPoint &globalPos)
{
    QMenu menu(this);
    QMenu *positionMenu = menu.addMenu(QStringLiteral("Translation Position"));
    QAction *aboveAction =
        positionMenu->addAction(QStringLiteral("Show Translation Above Source Subtitle"));
    aboveAction->setCheckable(true);
    aboveAction->setChecked(resultPosition_ == ResultPosition::AboveSource);

    QAction *belowAction =
        positionMenu->addAction(QStringLiteral("Show Translation Below Source Subtitle"));
    belowAction->setCheckable(true);
    belowAction->setChecked(resultPosition_ == ResultPosition::BelowSource);

    menu.addSeparator();
    QAction *togglePositionAction = menu.addAction(QStringLiteral("Toggle Position (Alt+T)"));

    QAction *picked = menu.exec(globalPos);
    if (picked == nullptr) {
        return;
    }

    if (picked == aboveAction) {
        resultPosition_ = ResultPosition::AboveSource;
        updateSubtitleLayout();
        updateWorkerScanZone();
        appendSubtitleLog(QStringLiteral("POSITION_CHANGED"), QStringLiteral("menu"),
                          QStringLiteral("Above"));
    } else if (picked == belowAction) {
        resultPosition_ = ResultPosition::BelowSource;
        updateSubtitleLayout();
        updateWorkerScanZone();
        appendSubtitleLog(QStringLiteral("POSITION_CHANGED"), QStringLiteral("menu"),
                          QStringLiteral("Below"));
    } else if (picked == togglePositionAction) {
        resultPosition_ = (resultPosition_ == ResultPosition::AboveSource)
                              ? ResultPosition::BelowSource
                              : ResultPosition::AboveSource;
        updateSubtitleLayout();
        updateWorkerScanZone();
        appendSubtitleLog(QStringLiteral("POSITION_CHANGED"), QStringLiteral("menu-toggle"),
                          resultPosition_ == ResultPosition::AboveSource ? QStringLiteral("Above")
                                                                         : QStringLiteral("Below"));
    }
}

QString OverlayWindow::subtitleKey(const QString &text) const
{
    QString key;
    key.reserve(text.size());
    for (const QChar c : text) {
        const ushort u = c.unicode();
        const bool isHan = (u >= 0x3400 && u <= 0x9FFF) || (u >= 0xF900 && u <= 0xFAFF);
        const bool isAsciiAlnum = (u >= '0' && u <= '9'); // Only get digits
        // const bool isAsciiAlnum = (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') ||
        //                           (u >= 'a' && u <= 'z'); // TODO: consider accented letters
        if (isHan || isAsciiAlnum) {
            key.append(c);
        }
    }
    return key;
}

static int longestCommonSubstring(const QString &left, const QString &right)
{
    const int n = left.size();
    const int m = right.size();

    int maxLen = 0;
    QVector<int> prev(m + 1, 0);
    QVector<int> curr(m + 1, 0);

    for (int i = 1; i <= n; ++i) {
        for (int j = 1; j <= m; ++j) {
            if (left.at(i - 1) == right.at(j - 1)) {
                curr[j] = prev[j - 1] + 1;
                maxLen = std::max(maxLen, curr[j]);
            } else {
                curr[j] = 0;
            }
        }
        std::swap(prev, curr);
        std::fill(curr.begin(), curr.end(), 0);
    }

    return maxLen;
}

static int longestCommonSubsequence(const QString &left, const QString &right)
{
    if (left.size() < right.size()) {
        return longestCommonSubsequence(right, left);
    }

    const int n = left.size();
    const int m = right.size();

    QVector<int> dp(m + 1, 0);

    for (int i = 1; i <= n; ++i) {
        int prev = 0;
        for (int j = 1; j <= m; ++j) {
            int temp = dp[j]; // Save the current value before updating
            if (left.at(i - 1) == right.at(j - 1)) {
                dp[j] = prev + 1;
            } else {
                dp[j] = std::max(dp[j], dp[j - 1]);
            }
            prev = temp; // Update prev to the saved value for the next iteration
        }
    }

    return dp[m];
}

static int levenshteinDistance(const QString &left, const QString &right)
{
    const int n = left.size();
    const int m = right.size();

    if (n == 0)
        return m;
    if (m == 0)
        return n;

    QVector<int> prev(m + 1);
    QVector<int> curr(m + 1);

    for (int j = 0; j <= m; ++j) {
        prev[j] = j;
    }

    for (int i = 1; i <= n; ++i) {
        curr[0] = i;

        for (int j = 1; j <= m; ++j) {
            const int cost = (left.at(i - 1) == right.at(j - 1)) ? 0 : 1;

            curr[j] = std::min({
                prev[j] + 1,       // delete
                curr[j - 1] + 1,   // insert
                prev[j - 1] + cost // replace
            });
        }

        std::swap(prev, curr);
    }

    return prev[m];
}

bool OverlayWindow::isLikelySameSubtitle(const QString &left, const QString &right) const
{
    if (left.isEmpty() || right.isEmpty()) {
        return false;
    }

    if (left == right) {
        return true;
    }

    const int minLen = std::min(left.size(), right.size());
    const int maxLen = std::max(left.size(), right.size());
    const int lenDiff = std::abs(left.size() - right.size());

    if (lenDiff > 3) {
        return false;
    }

    // P2: Improved substring detection for fuzzy matching
    // For short strings, allow substring containment more aggressively
    if (minLen >= 2 && lenDiff <= 2) {
        // Check if shorter string is contained in longer string
        const QString &shorter = (left.size() < right.size()) ? left : right;
        const QString &longer = (left.size() < right.size()) ? right : left;

        if (longer.contains(shorter)) {
            // qDebug() << "[FUZZY MATCH] Substring detected:" << shorter << "in" << longer;
            return true;
        }
    }

    // Allow containment only for sufficiently long strings with very small length difference.
    if ((left.contains(right) || right.contains(left)) && minLen >= 6 && lenDiff <= 1) {
        return true;
    }

    const int lev = levenshteinDistance(left, right);

    // P2: Improved levenshtein matching for short text
    if (minLen <= 1) {
        return lev == 0;  // Perfect match only for single char
    }

    if (minLen <= 3) {
        // For 2-3 chars: allow 1 edit (more lenient than before which required 0)
        const bool matched = lev <= 1;
        // if (matched && lev > 0) {
        //     qDebug() << "[FUZZY MATCH] Levenshtein distance 1 accepted:" << left << "|" << right;
        // }
        return matched;
    }

    // For 4+ chars: single-edit distance is unambiguously the same subtitle.
    // Also check for shared contiguous core: OCR noise often corrupts only the prefix/suffix
    // while leaving a central block intact (e.g. "豪司令员业" vs "夏司令员" share "司令员").
    // If the longest common substring covers >= 60% of the shorter string and is >= 3 chars,
    // treat the two readings as the same subtitle.
    if (lev <= 1) {
        return true;
    }

    const int lcs_sub = longestCommonSubstring(left, right);
    if (lcs_sub >= 3 && static_cast<double>(lcs_sub) / minLen >= 0.60) {
        // qDebug() << "[FUZZY MATCH] Common substring" << lcs_sub << "chars:" << left << "|" << right;
        return true;
    }

    if (minLen <= 4) {
        return false;  // No further heuristics for very short strings
    }

    const int lcs = longestCommonSubsequence(left, right);
    const double lcsRatio = static_cast<double>(lcs) / static_cast<double>(std::max(1, maxLen));
    const double levRatio = static_cast<double>(lev) / static_cast<double>(std::max(1, maxLen));

    const bool almostContained = (lcsRatio >= 0.82) && (levRatio <= 0.24);

    // if (almostContained) qDebug() << left << right << lcs << almostContained;

    return almostContained;
}

bool OverlayWindow::shouldDispatchSubtitle(const QString &ocrText)
{
    const QString key = subtitleKey(ocrText);
    if (key.isEmpty()) {
        return false;
    }

    if (!subtitleVisible_) {
        subtitleVisible_ = true;

        // if (wasRecentlyDispatched(key)) {
        //     return false;
        // }

        lastDispatchedSubtitleKey_ = key;
        rememberDispatchedSubtitle(key);
        subtitleDispatchTimer_.restart();
        return true;
    }

    if (wasRecentlyDispatched(key)) {
        return false;
    }

    if (subtitleDispatchTimer_.isValid() &&
        subtitleDispatchTimer_.elapsed() < tuning::kSubtitleSwitchCooldownMs) {
        return false;
    }

    const bool isRepeatKey = (key == lastDispatchedSubtitleKey_);
    if (isRepeatKey && subtitleDispatchTimer_.isValid() &&
        subtitleDispatchTimer_.elapsed() < tuning::kSubtitleResendCooldownMs) {
        return false;
    }

    // qDebug()
    // << "\nOCR =" << key
    // << "\nLAST =" << lastDispatchedSubtitleKey_
    // << "\nVISIBLE =" << subtitleVisible_;

    lastDispatchedSubtitleKey_ = key;
    rememberDispatchedSubtitle(key);
    subtitleDispatchTimer_.restart();

    return true;
}

bool OverlayWindow::wasRecentlyDispatched(const QString &key) const
{
    for (const QString &oldKey : recentSubtitleKeys_) {
        if (isLikelySameSubtitle(key, oldKey)) {
            // qDebug() << "RECENT MATCH:" << key << "<->" << oldKey;
            return true;
        }
    }

    return false;
}

void OverlayWindow::rememberDispatchedSubtitle(const QString &key)
{
    recentSubtitleKeys_.enqueue(key);

    while (recentSubtitleKeys_.size() > tuning::kRecentSubtitleWindowSize) {
        recentSubtitleKeys_.dequeue();
    }
}

QString OverlayWindow::mostFrequentCandidate() const
{
    QString best = candidateText_;
    int bestCount = -1;
    int bestQuality = candidateQualityScore(best);
    for (auto it = candidateFrequency_.cbegin(); it != candidateFrequency_.cend(); ++it) {
        const QString candidate = it.key();
        const int count = it.value();
        const int quality = candidateQualityScore(candidate);
        if (count > bestCount ||
            (count == bestCount && quality > bestQuality) ||
            (count == bestCount && quality == bestQuality && candidate.size() > best.size())) {
            best = candidate;
            bestCount = it.value();
            bestQuality = quality;
        }
    }
    return best;
}

Qt::Edges OverlayWindow::hitTestEdges(const QPoint &localPos) const
{
    Qt::Edges edges;
    if (localPos.x() <= resizeMarginPx_) {
        edges |= Qt::LeftEdge;
    }
    if (localPos.x() >= width() - resizeMarginPx_) {
        edges |= Qt::RightEdge;
    }
    if (localPos.y() <= resizeMarginPx_) {
        edges |= Qt::TopEdge;
    }
    if (localPos.y() >= height() - resizeMarginPx_) {
        edges |= Qt::BottomEdge;
    }
    return edges;
}

void OverlayWindow::updateCursorForPosition(const QPoint &localPos)
{
    const Qt::Edges edges = hitTestEdges(localPos);
    if ((edges & Qt::TopEdge && edges & Qt::LeftEdge) ||
        (edges & Qt::BottomEdge && edges & Qt::RightEdge)) {
        setCursor(Qt::SizeFDiagCursor);
    } else if ((edges & Qt::TopEdge && edges & Qt::RightEdge) ||
               (edges & Qt::BottomEdge && edges & Qt::LeftEdge)) {
        setCursor(Qt::SizeBDiagCursor);
    } else if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
        setCursor(Qt::SizeHorCursor);
    } else if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
        setCursor(Qt::SizeVerCursor);
    } else {
        unsetCursor();
    }
}

void OverlayWindow::applyResize(const QPoint &globalPos)
{
    QRect next = initialGeometry_;
    const QPoint delta = globalPos - initialMouseGlobalPos_;

    if (activeResizeEdges_ & Qt::LeftEdge) {
        next.setLeft(next.left() + delta.x());
    }
    if (activeResizeEdges_ & Qt::RightEdge) {
        next.setRight(next.right() + delta.x());
    }
    if (activeResizeEdges_ & Qt::TopEdge) {
        next.setTop(next.top() + delta.y());
    }
    if (activeResizeEdges_ & Qt::BottomEdge) {
        next.setBottom(next.bottom() + delta.y());
    }

    if (next.width() < minimumWidth()) {
        if (activeResizeEdges_ & Qt::LeftEdge) {
            next.setLeft(next.right() - minimumWidth());
        } else {
            next.setWidth(minimumWidth());
        }
    }

    if (next.height() < minimumHeight()) {
        if (activeResizeEdges_ & Qt::TopEdge) {
            next.setTop(next.bottom() - minimumHeight());
        } else {
            next.setHeight(minimumHeight());
        }
    }

    setGeometry(next);
}

QRect OverlayWindow::computeCaptureZone() const
{
    const QRect full = frameGeometry();
    const QRect localZone = localScanZoneRect();
    const int zoneX = full.x() + localZone.x();
    const int zoneY = full.y() + localZone.y();
    const int zoneW = localZone.width();
    const int zoneH = localZone.height();
    return QRect(zoneX, zoneY, zoneW, zoneH);
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
    candidateText_.clear();
    candidateFrequency_.clear();

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
        // Queue is drained and the source subtitle has disappeared — clear the overlay.
        if (!subtitleLabel_->text().isEmpty()) {
            subtitleLabel_->clear();
            appendSubtitleLog(QStringLiteral("DISPLAY_CLEARED"), QString(), QString());
        }
    }
}

void OverlayWindow::showTranslationEntry(const TranslationEntry &entry)
{
    lastTranslation_          = entry.translatedText;
    currentDisplayDurationMs_ = computeDisplayDurationMs(entry.sourceText);
    currentDisplayTimer_.restart();
    displayingTranslation_    = true;

    subtitleLabel_->setText(entry.translatedText);
    updateSubtitleLayout();
    updateWorkerScanZone();

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
