/**
 * - Create a C++ Qt6 widget named OverlayWindow that acts as the UI overlay.
 *   Set window flags so it is frameless, stays on top (Qt::WindowStaysOnTopHint), and has a translucent background (Qt::WA_TranslucentBackground).
 * - Draw a thin red border in paintEvent to show the active scanning region.
 * - Put a QLabel in the center with a semi-transparent black background, large bold font, and green color to display the Vietnamese subtitles.
 * - Connect the components: When CaptureWorker emits an image, send it to OcrEngine. If text is found, send it to TranslateClient.
 *   When TranslateClient returns the Vietnamese translation, update the QLabel text.
 */
