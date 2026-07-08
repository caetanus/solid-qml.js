#pragma once

#include <QObject>
#include <QQuickTextDocument>
#include <qqml.h>

// Syntax highlighting behind the <CodeEditor> module (solidqml.Widgets.Code). The current
// library for this is KSyntaxHighlighting — Kate's engine (KF6, 300+ syntax definitions);
// Scintilla has no QtQuick port. The dependency is OPTIONAL at build time: without it the
// handler reports available=false and the editor stays a plain mono editor with its gutter.
// Registered by the loader as `CodeHighlighter` under the `solidqml.native` uri.
namespace KSyntaxHighlighting { class SyntaxHighlighter; }

class CodeHighlighter : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(CodeHighlighter)
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    // Build-time capability: KSyntaxHighlighting found → real highlighting.
    Q_PROPERTY(bool available READ available CONSTANT)

public:
    explicit CodeHighlighter(QObject *parent = nullptr);

    QQuickTextDocument *document() const { return m_document; }
    void setDocument(QQuickTextDocument *doc);

    QString language() const { return m_language; }
    void setLanguage(const QString &lang);

    bool available() const;

signals:
    void documentChanged();
    void languageChanged();

private:
    void apply();

    QQuickTextDocument *m_document = nullptr;
    QString m_language;
    KSyntaxHighlighting::SyntaxHighlighter *m_highlighter = nullptr;
};
