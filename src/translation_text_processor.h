#pragma once

#include <QPair>
#include <QString>
#include <QVector>

#include "source_language.h"

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
    // The English-source counterpart of ResidualHan: a mostly-Vietnamese line that still
    // carries a few untranslated English words. Recoverable by dropping those words, the
    // same way ResidualHan is recoverable by dropping the stray Han characters.
    ResidualEnglish,
    // The English-source counterpart of ContainsHan: the model echoed the source line back
    // instead of translating it, which with a Latin-script source cannot be detected by
    // script alone.
    CopiedEnglishSource,
    EnglishHeavy,
    OverExpanded,
    TooShort,
    TooLong,
    Repeated,
    UnexpectedEnglish,
    LowScore,
};

// How much meaning the source line carries, in language-neutral units: Han characters for
// Chinese, whitespace-delimited words for English. Every translation length guard is
// expressed against this rather than against a raw character count, because one Han
// character is worth roughly one English word.
int sourceUnitCount(const QString &sourceText, SourceLanguage language);

// Text validation
bool isLikelySourceSubtitle(const QString &text, SourceLanguage language);
bool isLikelyChineseSubtitle(const QString &text);
bool isLikelyEnglishSubtitle(const QString &text);
bool containsHanCharacters(const QString &text);
bool isEnglishHeavyOutput(const QString &text);
bool isSuspiciouslyShortTranslation(const QString &sourceText, const QString &translatedText,
                                    SourceLanguage language);
bool isOverExpandedTranslation(const QString &sourceText, const QString &translatedText,
                               SourceLanguage language);
// True when the output is essentially the English source echoed back rather than
// translated. Only meaningful for an English source.
bool isEchoOfEnglishSource(const QString &sourceText, const QString &translatedText);

TranslationIssue evaluateTranslationQuality(const QString &sourceText,
                                            const QString &translatedText,
                                            SourceLanguage language);
QString translationIssueMessage(TranslationIssue issue);

// Remove <think>...</think> reasoning blocks (Qwen3 and other reasoning models) and any
// stray think tags, so the downstream pipeline only sees the actual answer. Safe to call
// on output from non-reasoning backends (no-op when no tags are present).
QString stripReasoningBlocks(QString text);

// Text sanitization
QString selectBestVietnameseLine(const QString &text, SourceLanguage language);
QString normalizeTranslation(QString text);
QString sanitizeFinalTranslation(QString text);
QString removeHanCharacters(QString text);
// Drop isolated non-Vietnamese Latin words (e.g. "prostration", "stupefied") while leaving
// Vietnamese syllables, proper names and numbers intact. This is the English pipeline's
// equivalent of removeHanCharacters(): with a Latin-script source there is no script
// boundary to clean along, so untranslated leftovers are identified by syllable shape.
QString removeForeignLatinWords(QString text);
QString postProcessTranslation(QString text);

// Best-effort recovery: extract the richest usable Vietnamese fragment from a raw
// backend output that failed the quality gate (mixed Han/English/meta text). Returns
// an empty string only when nothing Vietnamese-looking survives cleaning. Used as a
// last resort so a hard-to-translate line degrades to a partial subtitle instead of
// disappearing entirely.
QString salvageVietnameseFragment(const QString &rawText);

// Glossary
AliasData loadAliasRules(const QString &path);
QString applyAliasNormalization(const QString &translatedText,
                                   const QVector<QPair<QString, QString>> &aliasPairs);
QString buildGlossaryBlockForSource(const QString &sourceText,
                                    const QVector<QPair<QString, QString>> &glossaryPairs);

// Prompt builders. The two languages get genuinely different instructions — the Chinese
// prompt asks for Sino-Vietnamese readings of proper names, whereas the English prompt
// must ask for the opposite (Western names stay in Latin script) — so each has its own
// text rather than one template with substitutions.
QString translationPrompt(const QString &sourceText,
                          const QString &recentDialogueContext,
                          const QString &glossaryBlock,
                          SourceLanguage language);

// Prompt builder for retrying translation with previous translation context.
QString translationRetryPrompt(const QString &sourceText,
                              const QString &recentDialogueContext,
                              const QString &previousTranslation,
                              const QString &glossaryBlock,
                              TranslationIssue issue,
                              SourceLanguage language);

// Utilities
QString shortText(const QString &text, int maxChars);

} // namespace TranslationTextProcessor
