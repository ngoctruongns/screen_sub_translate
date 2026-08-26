#pragma once

#include <QMetaType>
#include <QString>

// Source language of the on-screen subtitles that get translated into Vietnamese.
//
// Each language drives its OWN pipeline: a dedicated PaddleOCR recognition model +
// charset, its own OCR text normalization, its own stabilization/dedupe rules, and
// its own prompt, glossary and translation quality gate. Anything that could be made
// to serve both languages takes this enum as a parameter; anything that could not
// (OCR normalization, incomplete-phrase detection, prompt text) has a separate
// implementation per language.
//
// All per-language constants live in tuning::LanguageProfile (tuning_params.h).
enum class SourceLanguage
{
    Chinese,
    English,
};

namespace sourcelang
{

inline constexpr SourceLanguage kDefault = SourceLanguage::Chinese;

// Stable identifier persisted in QSettings and accepted by translation_backend.json.
inline QString key(SourceLanguage language)
{
    return language == SourceLanguage::English ? QStringLiteral("en") : QStringLiteral("zh");
}

inline SourceLanguage fromKey(const QString &text, SourceLanguage fallback = kDefault)
{
    const QString normalized = text.trimmed().toLower();
    if (normalized == QStringLiteral("en") || normalized == QStringLiteral("eng") ||
        normalized == QStringLiteral("english")) {
        return SourceLanguage::English;
    }
    if (normalized == QStringLiteral("zh") || normalized == QStringLiteral("cn") ||
        normalized == QStringLiteral("chinese")) {
        return SourceLanguage::Chinese;
    }
    return fallback;
}

inline QString displayName(SourceLanguage language)
{
    return language == SourceLanguage::English ? QStringLiteral("English")
                                               : QStringLiteral("Chinese");
}

} // namespace sourcelang

// Crossing a queued signal/slot connection (controller thread -> OCR thread).
Q_DECLARE_METATYPE(SourceLanguage)
