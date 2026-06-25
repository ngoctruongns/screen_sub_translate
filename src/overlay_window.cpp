#include "overlay_window.h"

#include <algorithm>
#include <cstdlib>

#include <QAction>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFontMetrics>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QShortcut>
#include <QTextStream>

OverlayWindow::OverlayWindow(QWidget *parent) : QWidget(parent)
{
    qRegisterMetaType<cv::Mat>("cv::Mat");

    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
    setMinimumSize(500, 120);
    resize(900, 200);

    setupUi();
    setupHotkeys();
    move(400, 780);
    logFilePath_ = QCoreApplication::applicationDirPath() + QStringLiteral("/subtitle_log.txt");
    ocrAcceptedTimer_.start();
    appendSubtitleLog(QStringLiteral("SESSION_START"), QString(), QString());

    captureWorker_ = new CaptureWorker(frameGeometry());
    captureWorker_->moveToThread(&captureThread_);
    ocrWorker_ = new OcrWorker();
    ocrWorker_->moveToThread(&ocrThread_);

    connect(&captureThread_, &QThread::started, captureWorker_, &CaptureWorker::start);
    connect(&captureThread_, &QThread::finished, captureWorker_, &QObject::deleteLater);
    connect(&ocrThread_, &QThread::finished, ocrWorker_, &QObject::deleteLater);
    connect(captureWorker_, &CaptureWorker::imageProcessed, this, &OverlayWindow::onImageProcessed);
    connect(ocrWorker_, &OcrWorker::ocrReady, this, &OverlayWindow::onOcrReady);
    connect(ocrWorker_, &OcrWorker::ocrError, this, &OverlayWindow::onOcrError);
    connect(&translateClient_, &TranslateClient::translationReady, this,
            &OverlayWindow::onTranslationReady);
    connect(&translateClient_, &TranslateClient::translationError, this,
            &OverlayWindow::onTranslationError);
        connect(&translateClient_, &TranslateClient::backendChanged, this,
            [this](const QString &backendName) {
            appendSubtitleLog(QStringLiteral("TRANSLATE_BACKEND_CHANGED"),
                      QStringLiteral("runtime"), backendName);
            });

    applyNoiseProfile(noiseProfile_);
    captureThread_.start();
    ocrThread_.start();
}

OverlayWindow::~OverlayWindow()
{
    if (captureWorker_) {
        QMetaObject::invokeMethod(captureWorker_, "stop", Qt::QueuedConnection);
    }
    captureThread_.quit();
    ocrThread_.quit();
    captureThread_.wait();
    ocrThread_.wait();
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
    painter.setPen(QPen(QColor(255, 180, 0, 180), 1, Qt::DashLine));
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
        ++emptyOcrStreak_;
        if (emptyOcrStreak_ >= tuning::kSubtitleDisappearEmptyFrames) {
            subtitleVisible_ = false;
        }
    } else {
        emptyOcrStreak_ = 0;

        if (ocrText == lastCandidateOcr_) {
            ++candidateRepeatCount_;
        } else {
            lastCandidateOcr_ = ocrText;
            candidateRepeatCount_ = 1;
        }

        const bool enoughLength = ocrText.size() >= minOcrLength_;
        const bool stableEnough = candidateRepeatCount_ >= requiredRepeatCount_;
        const bool enoughGap = ocrAcceptedTimer_.elapsed() >= minOcrAcceptGapMs_;

        if (enoughLength && stableEnough && enoughGap && shouldDispatchSubtitle(ocrText)) {
            lastOcrText_ = ocrText;
            ocrAcceptedTimer_.restart();
            appendSubtitleLog(QStringLiteral("OCR_DETECTED"), ocrText, QString());
            translateClient_.requestTranslation(ocrText);
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
    subtitleLabel_->setText(error);
    appendSubtitleLog(QStringLiteral("OCR_ERROR"), QString::number(requestId), error);
    lastCandidateOcr_.clear();
    candidateRepeatCount_ = 0;
    ++emptyOcrStreak_;
    if (emptyOcrStreak_ >= tuning::kSubtitleDisappearEmptyFrames) {
        subtitleVisible_ = false;
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

    if (sourceText != lastOcrText_) {
        appendSubtitleLog(QStringLiteral("TRANSLATED_STALE"), sourceText, translatedText);
    }

    lastTranslation_ = translatedText;
    subtitleLabel_->setText(translatedText);
    updateSubtitleLayout();
    appendSubtitleLog(QStringLiteral("TRANSLATED"), sourceText, translatedText);
}

void OverlayWindow::onTranslationError(const QString &error)
{
    // Keep the last subtitle on screen to avoid flicker during transient network failures.
    appendSubtitleLog(QStringLiteral("TRANSLATE_ERROR"), lastOcrText_, error);
}

void OverlayWindow::setupUi()
{
    subtitleLabel_ = new QLabel(
        QStringLiteral("Right-click: options | Alt+1 Fast Alt+2 Balanced Alt+3 Clean"),
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
    auto *fastShortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+1")), this);
    auto *balancedShortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+2")), this);
    auto *cleanShortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+3")), this);
    auto *positionToggleShortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+T")), this);

    connect(fastShortcut, &QShortcut::activated, this,
            [this]() { applyNoiseProfile(NoiseProfile::Fast); });
    connect(balancedShortcut, &QShortcut::activated, this,
            [this]() { applyNoiseProfile(NoiseProfile::Balanced); });
    connect(cleanShortcut, &QShortcut::activated, this,
            [this]() { applyNoiseProfile(NoiseProfile::Clean); });
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

    QMetaObject::invokeMethod(captureWorker_, "setScanZone", Qt::QueuedConnection,
                              Q_ARG(QRect, computeCaptureZone()));
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

    QMenu *profileMenu = menu.addMenu(QStringLiteral("Noise Profile"));
    QAction *fastAction = profileMenu->addAction(QStringLiteral("Fast  (Alt+1)"));
    fastAction->setCheckable(true);
    fastAction->setChecked(noiseProfile_ == NoiseProfile::Fast);

    QAction *balancedAction = profileMenu->addAction(QStringLiteral("Balanced  (Alt+2)"));
    balancedAction->setCheckable(true);
    balancedAction->setChecked(noiseProfile_ == NoiseProfile::Balanced);

    QAction *cleanAction = profileMenu->addAction(QStringLiteral("Clean  (Alt+3)"));
    cleanAction->setCheckable(true);
    cleanAction->setChecked(noiseProfile_ == NoiseProfile::Clean);

    QMenu *backendMenu = menu.addMenu(QStringLiteral("Translation Backend"));
    QAction *localBackendAction =
        backendMenu->addAction(QStringLiteral("Local Light Model (OPUS)"));
    localBackendAction->setCheckable(true);
    localBackendAction->setChecked(translateClient_.backend() == TranslateClient::Backend::Local);

    QAction *googleBackendAction =
        backendMenu->addAction(QStringLiteral("Google Translate API"));
    googleBackendAction->setCheckable(true);
    googleBackendAction->setChecked(translateClient_.backend() == TranslateClient::Backend::GoogleApi);

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
    } else if (picked == fastAction) {
        applyNoiseProfile(NoiseProfile::Fast);
    } else if (picked == balancedAction) {
        applyNoiseProfile(NoiseProfile::Balanced);
    } else if (picked == cleanAction) {
        applyNoiseProfile(NoiseProfile::Clean);
    } else if (picked == localBackendAction) {
        translateClient_.setBackend(TranslateClient::Backend::Local);
        appendSubtitleLog(QStringLiteral("TRANSLATE_BACKEND_CHANGED"), QStringLiteral("menu"),
                          QStringLiteral("local"));
    } else if (picked == googleBackendAction) {
        translateClient_.setBackend(TranslateClient::Backend::GoogleApi);
        appendSubtitleLog(QStringLiteral("TRANSLATE_BACKEND_CHANGED"), QStringLiteral("menu"),
                          QStringLiteral("google"));
    }
}

QString OverlayWindow::subtitleKey(const QString &text) const
{
    QString key;
    key.reserve(text.size());
    for (const QChar c : text) {
        const ushort u = c.unicode();
        const bool isHan = (u >= 0x3400 && u <= 0x9FFF) || (u >= 0xF900 && u <= 0xFAFF);
        const bool isAsciiAlnum = (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') ||
                                  (u >= 'a' && u <= 'z');
        if (isHan || isAsciiAlnum) {
            key.append(c);
        }
    }
    return key;
}

bool OverlayWindow::isLikelySameSubtitle(const QString &left, const QString &right) const
{
    if (left.isEmpty() || right.isEmpty()) {
        return false;
    }

    if (left == right) {
        return true;
    }

    if (left.contains(right) || right.contains(left)) {
        return true;
    }

    const int minLen = std::min(left.size(), right.size());
    if (minLen <= 1) {
        return false;
    }

    int samePos = 0;
    for (int i = 0; i < minLen; ++i) {
        if (left.at(i) == right.at(i)) {
            ++samePos;
        }
    }

    const double ratio = static_cast<double>(samePos) / static_cast<double>(minLen);
    const int lenDiff = std::abs(left.size() - right.size());
    return ratio >= 0.72 && lenDiff <= 2;
}

bool OverlayWindow::shouldDispatchSubtitle(const QString &ocrText)
{
    const QString key = subtitleKey(ocrText);
    if (key.isEmpty()) {
        return false;
    }

    if (!subtitleVisible_) {
        subtitleVisible_ = true;
        lastDispatchedSubtitleKey_ = key;
        subtitleDispatchTimer_.restart();
        return true;
    }

    if (isLikelySameSubtitle(key, lastDispatchedSubtitleKey_)) {
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

    lastDispatchedSubtitleKey_ = key;
    subtitleDispatchTimer_.restart();
    return true;
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
    QFile file(logFilePath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    const QString now =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    QString sourceSafe = sourceText;
    QString translatedSafe = translatedText;

    QTextStream out(&file);
    out << '[' << now << "] " << status << " | source=" << sourceSafe.replace('\n', ' ')
        << " | result=" << translatedSafe.replace('\n', ' ') << '\n';
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

void OverlayWindow::applyNoiseProfile(NoiseProfile profile)
{
    noiseProfile_ = profile;

    const tuning::NoiseProfileConfig config =
        (profile == NoiseProfile::Fast)
            ? tuning::kFastProfile
            : ((profile == NoiseProfile::Balanced) ? tuning::kBalancedProfile
                                                   : tuning::kCleanProfile);

    const double changeThreshold = config.changeThreshold;
    const double minChangedRatio = config.minChangedRatio;
    const double minStdDev = config.minStdDev;
    minOcrLength_ = config.minOcrLength;
    requiredRepeatCount_ = config.requiredRepeatCount;
    minOcrAcceptGapMs_ = config.minOcrAcceptGapMs;

    candidateRepeatCount_ = 0;
    lastCandidateOcr_.clear();

    if (captureWorker_) {
        QMetaObject::invokeMethod(captureWorker_, "setNoiseParams", Qt::QueuedConnection,
                                  Q_ARG(double, changeThreshold), Q_ARG(double, minChangedRatio),
                                  Q_ARG(double, minStdDev));
    }

    appendSubtitleLog(QStringLiteral("PROFILE_CHANGED"), noiseProfileName(profile),
                      QStringLiteral("len=%1 repeat=%2 gap=%3ms")
                          .arg(minOcrLength_)
                          .arg(requiredRepeatCount_)
                          .arg(minOcrAcceptGapMs_));
}

QString OverlayWindow::noiseProfileName(NoiseProfile profile) const
{
    if (profile == NoiseProfile::Fast) {
        return QStringLiteral("Fast");
    }
    if (profile == NoiseProfile::Balanced) {
        return QStringLiteral("Balanced");
    }
    return QStringLiteral("Clean");
}
