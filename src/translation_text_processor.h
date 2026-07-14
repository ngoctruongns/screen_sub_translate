#pragma once

#include <QPair>
#include <QString>
#include <QVector>

namespace TranslationTextProcessor
{

struct AliasData {
    QVector<QPair<QString, QString>> glossaryPairs;
    QVector<QPair<QString, QString>> aliasPairs;
};

enum class TranslationIssue
{
    None,
    Empty,
    NoUsableVietnameseCandidate,
    ContainsHan,
    ResidualHan,
    EnglishHeavy,
    OverExpanded,
    TooShort,
    TooLong,
    Repeated,
    UnexpectedEnglish,
    LowScore,
};

// Text validation
bool isLikelyChineseSubtitle(const QString &text);
bool containsHanCharacters(const QString &text);
bool isEnglishHeavyOutput(const QString &text);
bool isSuspiciouslyShortTranslation(const QString &sourceText, const QString &translatedText);
bool isOverExpandedTranslation(const QString &sourceText, const QString &translatedText);

TranslationIssue evaluateTranslationQuality(const QString &sourceText, const QString &translatedText);
QString translationIssueMessage(TranslationIssue issue);

// Text sanitization
QString selectBestVietnameseLine(const QString &text);
QString normalizeTranslation(QString text);
QString sanitizeFinalTranslation(QString text);
QString removeHanCharacters(QString text);
QString postProcessTranslation(QString text);

// Glossary
AliasData loadAliasRules(const QString &path);
QString loadPromptContext(const QString &path);
QString applyAliasNormalization(const QString &translatedText,
                                   const QVector<QPair<QString, QString>> &aliasPairs);

// Prompt builders
QString translationPrompt(const QString &sourceText,
                          const QString &contextBlock,
                          const QString &recentDialogueContext,
                          const QString &glossaryBlock);

// Prompt builder for retrying translation with previous translation context.
QString translationRetryPrompt(const QString &sourceText,
                              const QString &recentDialogueContext,
                              const QString &previousTranslation,
                              const QString &glossaryBlock,
                              TranslationIssue issue);

// Utilities
QString shortText(const QString &text, int maxChars);

} // namespace TranslationTextProcessor
