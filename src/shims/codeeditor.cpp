#include "codeeditor.h"

namespace {

// A light, GitHub-ish palette — readable on the module's white editor body.
QTextCharFormat fmtColor(const char *color, bool bold = false)
{
    QTextCharFormat f;
    f.setForeground(QColor(QLatin1String(color)));
    if (bold)
        f.setFontWeight(QFont::Bold);
    return f;
}

const char *kKeyword = "#cf222e";
const char *kString = "#0a3069";
const char *kComment = "#6e7781";
const char *kNumber = "#0550ae";
const char *kFunction = "#8250df";
const char *kType = "#953800";

QString jsKeywords()
{
    return QStringLiteral(
        "\\b(?:async|await|break|case|catch|class|const|continue|debugger|default|delete|do|else|"
        "enum|export|extends|finally|for|function|if|implements|import|in|instanceof|interface|let|"
        "new|of|return|static|super|switch|this|throw|try|type|typeof|var|void|while|with|yield|"
        "true|false|null|undefined)\\b");
}

} // namespace

void SolidSyntaxRules::setLanguage(const QString &lang)
{
    m_rules.clear();
    m_commentStart = QRegularExpression();
    m_commentEnd = QRegularExpression();
    m_commentFormat = fmtColor(kComment);
    m_commentFormat.setFontItalic(true);

    const QString l = lang.toLower();
    const bool js = l == QLatin1String("javascript") || l == QLatin1String("js")
        || l == QLatin1String("typescript") || l == QLatin1String("ts") || l.isEmpty();
    const bool qml = l == QLatin1String("qml");
    const bool css = l == QLatin1String("css");
    const bool json = l == QLatin1String("json");

    const auto add = [this](const QString &pattern, const QTextCharFormat &f) {
        m_rules.append({ QRegularExpression(pattern), f });
    };

    if (js || qml) {
        add(jsKeywords(), fmtColor(kKeyword));
        if (qml) {
            add(QStringLiteral("\\b(?:property|signal|readonly|required|alias|component|on\\w+)\\b"), fmtColor(kKeyword));
            add(QStringLiteral("\\b[A-Z]\\w*(?=\\s*\\{)"), fmtColor(kType, true)); // type instantiation
        }
        add(QStringLiteral("\\b[A-Za-z_$][\\w$]*(?=\\s*\\()"), fmtColor(kFunction));   // calls/decls
        add(QStringLiteral("\\b\\d+(?:\\.\\d+)?\\b"), fmtColor(kNumber));
        add(QStringLiteral("\"[^\"\\\\]*(?:\\\\.[^\"\\\\]*)*\""), fmtColor(kString)); // "…"
        add(QStringLiteral("'[^'\\\\]*(?:\\\\.[^'\\\\]*)*'"), fmtColor(kString));      // '…'
        add(QStringLiteral("`[^`]*`"), fmtColor(kString));                              // `…` (single line)
        add(QStringLiteral("//[^\n]*"), m_commentFormat);
        m_commentStart = QRegularExpression(QStringLiteral("/\\*"));
        m_commentEnd = QRegularExpression(QStringLiteral("\\*/"));
    } else if (css) {
        add(QStringLiteral("[.#]?[\\w-]+(?=\\s*\\{)"), fmtColor(kType, true));          // selectors
        add(QStringLiteral("\\b[\\w-]+(?=\\s*:)"), fmtColor(kFunction));                 // property names
        add(QStringLiteral("\\b\\d+(?:\\.\\d+)?(?:px|em|rem|vh|vw|%|s|ms)?\\b"), fmtColor(kNumber));
        add(QStringLiteral("#[0-9a-fA-F]{3,8}\\b"), fmtColor(kNumber));
        add(QStringLiteral("\"[^\"]*\"|'[^']*'"), fmtColor(kString));
        m_commentStart = QRegularExpression(QStringLiteral("/\\*"));
        m_commentEnd = QRegularExpression(QStringLiteral("\\*/"));
    } else if (json) {
        add(QStringLiteral("\"[^\"\\\\]*(?:\\\\.[^\"\\\\]*)*\"(?=\\s*:)"), fmtColor(kFunction)); // keys
        add(QStringLiteral("\"[^\"\\\\]*(?:\\\\.[^\"\\\\]*)*\"(?!\\s*:)"), fmtColor(kString));   // values
        add(QStringLiteral("\\b(?:true|false|null)\\b"), fmtColor(kKeyword));
        add(QStringLiteral("-?\\b\\d+(?:\\.\\d+)?(?:[eE][+-]?\\d+)?\\b"), fmtColor(kNumber));
    }

    rehighlight();
}

void SolidSyntaxRules::highlightBlock(const QString &text)
{
    for (const Rule &rule : std::as_const(m_rules)) {
        auto it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            setFormat(int(m.capturedStart()), int(m.capturedLength()), rule.format);
        }
    }

    // Multi-line comments (the canonical QSyntaxHighlighter block-state dance).
    setCurrentBlockState(0);
    if (m_commentStart.pattern().isEmpty())
        return;
    int start = 0;
    if (previousBlockState() != 1) {
        const auto m = m_commentStart.match(text);
        start = m.hasMatch() ? int(m.capturedStart()) : -1;
    }
    while (start >= 0) {
        const auto endMatch = m_commentEnd.match(text, start);
        if (!endMatch.hasMatch()) {
            setCurrentBlockState(1);
            setFormat(start, int(text.length()) - start, m_commentFormat);
            break;
        }
        const int len = int(endMatch.capturedEnd()) - start;
        setFormat(start, len, m_commentFormat);
        const auto next = m_commentStart.match(text, start + len);
        start = next.hasMatch() ? int(next.capturedStart()) : -1;
    }
}

CodeHighlighter::CodeHighlighter(QObject *parent)
    : QObject(parent)
{
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
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return;
    if (!m_highlighter)
        m_highlighter = new SolidSyntaxRules(this);
    m_highlighter->setDocument(doc);
    m_highlighter->setLanguage(m_language);
}
