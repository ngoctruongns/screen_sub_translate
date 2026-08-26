#include <QApplication>
#include <QDebug>
#include <QStringList>

#include "src/overlay_window.h"
#include "src/tuning_params.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Required for QSettings so the overlay windows can persist their geometry.
    QApplication::setOrganizationName(QStringLiteral("ScreenSubTranslator"));
    QApplication::setApplicationName(QStringLiteral("ScreenSubTranslator"));

    // Runtime tuning must be applied BEFORE the controller exists: it creates the worker
    // threads, and from that point on the tuning globals are read concurrently and must
    // not be written. A missing file is fine — the built-in defaults are a valid config.
    QStringList tuningMessages;
    const bool tuningLoaded =
        tuning::loadTuningConfig(QString::fromUtf8(tuning::kTuningConfigPath), &tuningMessages);
    qInfo().noquote() << (tuningLoaded
                              ? QStringLiteral("Tuning config loaded from %1")
                                    .arg(tuning::resolvedTuningConfigPath())
                              : QStringLiteral("Tuning config not applied; using built-in defaults"));
    for (const QString &message : tuningMessages) {
        qWarning().noquote() << "[tuning]" << message;
    }

    // The controller is not itself a visible widget; it creates and shows the two
    // independent overlay windows (OCR capture zone and translation display).
    OverlayWindow controller;

    return app.exec();
}
