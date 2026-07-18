#include <QApplication>

#include "src/overlay_window.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Required for QSettings so the overlay windows can persist their geometry.
    QApplication::setOrganizationName(QStringLiteral("ScreenSubTranslator"));
    QApplication::setApplicationName(QStringLiteral("ScreenSubTranslator"));

    // The controller is not itself a visible widget; it creates and shows the two
    // independent overlay windows (OCR capture zone and translation display).
    OverlayWindow controller;

    return app.exec();
}
