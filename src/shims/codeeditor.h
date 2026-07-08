#pragma once

#include <QObject>
#include <QQuickTextDocument>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <qqml.h>

// Syntax highlighting behind the <CodeEditor> module (solidqml.Widgets.Code) — OUR OWN
// QSyntaxHighlighter rules (owner directive: no KDE dependency; Scintilla has no QtQuick
// port). Pure QtGui, always available: keywords/strings/comments/numbers/functions per
// language (JavaScript & TypeScript, QML, CSS, JSON), with block-state multi-line comments.
// Registered by the loader as `CodeHighlighter` under the `solidqml.native` uri.
class SolidSyntaxRules;

class CodeHighlighter : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(CodeHighlighter)
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    // Kept for API stability with the earlier optional-dep design: the built-in rules are
    // always compiled in.
    Q_PROPERTY(bool available READ available CONSTANT)

public:
    explicit CodeHighlighter(QObject *parent = nullptr);

    QQuickTextDocument *document() const { return m_document; }
    void setDocument(QQuickTextDocument *doc);

    QString language() const { return m_language; }
    void setLanguage(const QString &lang);

    bool available() const { return true; }

signals:
    void documentChanged();
    void languageChanged();

private:
    void apply();

    QQuickTextDocument *m_document = nullptr;
    QString m_language;
    SolidSyntaxRules *m_highlighter = nullptr;
};

// The QSyntaxHighlighter half: one regex rule table per language, rebuilt on language change.
class SolidSyntaxRules final : public QSyntaxHighlighter {
    Q_OBJECT

public:
    using QSyntaxHighlighter::QSyntaxHighlighter;

    void setLanguage(const QString &lang);

protected:
    void highlightBlock(const QString &text) override;

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    QList<Rule> m_rules;
    // Multi-line comment delimiters (empty pattern = language has none, e.g. JSON).
    QRegularExpression m_commentStart;
    QRegularExpression m_commentEnd;
    QTextCharFormat m_commentFormat;
};
