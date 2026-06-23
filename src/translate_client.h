/**
 * Create a C++ class named TranslateClient that inherits from QObject.
 * - It should use QNetworkAccessManager to send a non-blocking asynchronous HTTP POST or GET request to a free translation API (like Google Translate free RPC endpoint) to translate Chinese text into Vietnamese.
 * - Parse the JSON response and emit a signal translationReady(const QString& translatedText). Ensure it handles network errors safely without crashing.
 */
