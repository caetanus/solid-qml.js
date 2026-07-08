#include "codeeditor.h"

#if defined(HAVE_KSYNTAX)
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

namespace {
// One repository per process: definition/theme lookup is not cheap.
KSyntaxHighlighting::Repository &repository()
{
    static KSyntaxHighlighting::Repository repo;
    return repo;
}
} // namespace
#endif

CodeHighlighter::CodeHighlighter(QObject *parent)
    : QObject(parent)
{
}

bool CodeHighlighter::available() const
{
#if defined(HAVE_KSYNTAX)
    return true;
#else
    return false;
#endif
}

void CodeHighlighter::setDocument(QQuickTextDocument *doc)
{
    if (m_document == doc)
        return;
    m_document = doc;
    emit documentChanged();
    apply();
}

void CodeHighlighter::setLanguage(const QString &lang)
{
    if (m_language == lang)
        return;
    m_language = lang;
    emit languageChanged();
    apply();
}

void CodeHighlighter::apply()
{
#if defined(HAVE_KSYNTAX)
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return;
    if (!m_highlighter)
        m_highlighter = new KSyntaxHighlighting::SyntaxHighlighter(this);
    m_highlighter->setDocument(doc);
    const auto def = repository().definitionForName(m_language.isEmpty() ? QStringLiteral("JavaScript")
                                                                         : m_language);
    m_highlighter->setDefinition(def.isValid() ? def
                                               : repository().definitionForFileName(QStringLiteral("f.") + m_language));
    m_highlighter->setTheme(repository().defaultTheme(KSyntaxHighlighting::Repository::LightTheme));
    m_highlighter->rehighlight();
#endif
}
