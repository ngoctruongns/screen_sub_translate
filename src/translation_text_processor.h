#pragma once

#include <QPair>
#include <QString>
#include <QVector>

namespace TranslationTextProcessor
{

struct GlossaryData {
    QString promptLines;
    QVector<QPair<QString, QString>> aliasPairs;
};

// Text validation
bool isLikelyChineseSubtitle(const QString &text);
bool containsHanCharacters(const QString &text);
bool isEnglishHeavyOutput(const QString &text);
bool isSuspiciouslyShortTranslation(const QString &sourceText, const QString &translatedText);
bool isOverExpandedTranslation(const QString &sourceText, const QString &translatedText);

// Text sanitization
QString normalizeTranslation(QString text);
QString sanitizeFinalTranslation(QString text);
QString postProcessTranslation(QString text);

// Glossary
GlossaryData loadGlossary(const QString &path);
QString loadPromptContext(const QString &path);
QString applyGlossaryNormalization(const QString &translatedText,
                                   const QVector<QPair<QString, QString>> &aliasPairs);

// Prompt builders
QString translationPrompt(const QString &sourceText,
                         const QString &contextBlock,
                         const QString &recentDialogueContext);
QString repairPrompt(const QString &sourceText,
                    const QString &draftTranslation,
                    const QString &contextBlock);
QString rescuePrompt(const QString &sourceText,
                    const QString &draftTranslation,
                    const QString &contextBlock,
                    const QString &recentDialogueContext);
QString completeLinePrompt(const QString &sourceText,
                          const QString &draftTranslation,
                          const QString &contextBlock,
                          const QString &recentDialogueContext);

// Utilities
QString shortText(const QString &text, int maxChars);
int hanCharCount(const QString &text);
int latinWordCount(const QString &text);

} // namespace TranslationTextProcessor
