#include <QApplication>

#include "src/overlay_window.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    OverlayWindow window;
    window.show();

    return app.exec();
}
