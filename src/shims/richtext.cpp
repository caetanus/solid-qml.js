#include "richtext.h"
#include "richtext_odf.h"

#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QQuickItem>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextBoundaryFinder>
#include <QTextDocumentFragment>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextImageFormat>
#include <QTextList>
#include <QTextTable>
#include <QAbstractUndoItem>
#include <QPointer>

RichTextHandler::RichTextHandler(QObject *parent) : QObject(parent)
{
    // The system clipboard, not this document: canPaste must track another app copying something
    // too (office item 2b, item 6).
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &RichTextHandler::canPasteChanged);
}

void RichTextHandler::setDocument(QQuickTextDocument *doc)
{
    if (m_document == doc)
        return;
    QObject::disconnect(m_modifiedConn);
    QObject::disconnect(m_contentsConn);
    QObject::disconnect(m_cullingConn);
    QObject::disconnect(m_headingsConn);
    m_document = doc;
    if (QTextDocument *td = doc ? doc->textDocument() : nullptr) {
        m_modifiedConn = connect(td, &QTextDocument::modificationChanged, this, &RichTextHandler::modifiedChanged);
        m_contentsConn = connect(td, &QTextDocument::contentsChanged, this, &RichTextHandler::contentsChanged);
        // Every edit can cross the threshold (a load, a paste, a select-all + delete). A load is
        // one edit block, so this runs once per load, not once per paragraph.
        m_cullingConn = connect(td, &QTextDocument::contentsChanged, this, &RichTextHandler::updateViewportCulling);
        // ★3: incremental outline tracking (position/charsRemoved/charsAdded — the SAME hook a
        // QSyntaxHighlighter uses; it fires after the content change, before layout).
        m_headingsConn = connect(td, &QTextDocument::contentsChange, this, &RichTextHandler::handleContentsChange);
    }
    updateViewportCulling();
    rebuildIndex(); // a full, but infrequent (document-attach-time) scan establishes the baseline
    emit documentChanged();
    emit formatChanged();
    emit modifiedChanged();
    // Same reasoning as loadOdf()/newDocument(): rebuildIndex() just computed wordCount/charCount
    // for the newly-attached document — tell any binding on them (office item 2b, item 7).
    emit contentsChanged();
}

namespace {

// A file:// URL or a plain path (what an app passes) → a filesystem path.
QString localPath(const QUrl &file)
{
    return file.isLocalFile() ? file.toLocalFile() : file.toString(QUrl::PreferLocalFile);
}

// Forward declaration: defined (and documented) next to defineStyle() further down, which shares
// it; newDocument(options) (office item 2b addendum) needs it earlier in the file.
QString buildStyle(const rt::StyleSheet &sheet, const QString &name, const QVariantMap &props, rt::NamedStyle *out);

} // namespace

void RichTextHandler::updateViewportCulling()
{
    // The QQuickTextDocument's parent is the TextEdit that owns it (QQuickTextEdit creates it with
    // `this`), the same item Qt's own setText rule flips.
    auto *editor = m_document ? qobject_cast<QQuickItem *>(m_document->parent()) : nullptr;
    const QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!editor || !doc)
        return;
    // characterCount() is O(1). Qt counts the setText() string (markup included for rich text);
    // the document's characters are what the culling saves rendering of.
    const bool large = doc->characterCount() > kLargeTextThreshold;
    if (editor->flags().testFlag(QQuickItem::ItemObservesViewport) != large)
        editor->setFlag(QQuickItem::ItemObservesViewport, large);
}

// ─── outline (office item 2b, ★3) ─────────────────────────────────────────────────────────────

namespace {
// item 7: a "word" is a QTextBoundaryFinder::Word segment starting with a letter or digit (so
// runs of whitespace/punctuation between words don't count); "chars" is simply the block's own
// text length — QTextBlock::text() already excludes the paragraph separator.
int countWords(const QString &text)
{
    if (text.isEmpty())
        return 0;
    QTextBoundaryFinder bf(QTextBoundaryFinder::Word, text);
    int count = 0;
    int prev = 0;
    for (bf.toStart(); bf.toNextBoundary() != -1; prev = bf.position()) {
        const QStringView seg = QStringView(text).mid(prev, bf.position() - prev);
        if (!seg.isEmpty() && seg.at(0).isLetterOrNumber())
            ++count;
    }
    return count;
}
} // namespace

void RichTextHandler::rebuildIndex()
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    const QList<HeadingEntry> before = m_headings;
    m_headings.clear();
    m_blockCounts.clear();
    m_totalChars = 0;
    m_totalWords = 0;
    if (doc) {
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            const QString text = b.text();
            const int chars = text.size();
            const int words = countWords(text);
            m_blockCounts << BlockCount { b, chars, words };
            m_totalChars += chars;
            m_totalWords += words;
            const int level = b.blockFormat().headingLevel();
            if (level > 0)
                m_headings << HeadingEntry { b, level, text };
        }
    }
    bool same = before.size() == m_headings.size();
    for (int i = 0; same && i < before.size(); ++i)
        same = before[i].block == m_headings[i].block && before[i].level == m_headings[i].level
            && before[i].text == m_headings[i].text;
    if (!same)
        emit headingsChanged();
}

void RichTextHandler::handleContentsChange(int position, int charsRemoved, int charsAdded)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return;
    const int last = doc->characterCount() - 1;
    const QTextBlock from = doc->findBlock(qBound(0, position, last));
    const QTextBlock to = doc->findBlock(qBound(0, position + charsAdded, last));
    const int fromNum = from.blockNumber(), toNum = to.blockNumber();
    Q_UNUSED(charsRemoved);

    // What the re-scanned span gives NOW (document order, since we walk from→to forward) — both
    // the headings in it and every block's counts (item 7), in the SAME walk.
    QList<HeadingEntry> rescannedHeadings;
    QList<BlockCount> rescannedCounts;
    qint64 newChars = 0, newWords = 0;
    for (QTextBlock b = from; b.isValid(); b = b.next()) {
        const QString text = b.text();
        const int chars = text.size();
        const int words = countWords(text);
        rescannedCounts << BlockCount { b, chars, words };
        newChars += chars;
        newWords += words;
        const int level = b.blockFormat().headingLevel();
        if (level > 0)
            rescannedHeadings << HeadingEntry { b, level, text };
        if (b == to)
            break;
    }

    // Splice m_headings: keep every OLD heading outside [from, to] and not deleted; everything
    // inside that span — deleted or not — is replaced by `rescannedHeadings`, inserted where the
    // span's old entries were (they are contiguous, since m_headings is kept in document order).
    QList<HeadingEntry> splicedHeadings;
    splicedHeadings.reserve(m_headings.size() + rescannedHeadings.size());
    int insertAt = -1;
    for (const HeadingEntry &e : std::as_const(m_headings)) {
        const bool invalid = !e.block.isValid();
        const bool inSpan = !invalid && e.block.blockNumber() >= fromNum && e.block.blockNumber() <= toNum;
        if (invalid || inSpan) {
            if (insertAt < 0)
                insertAt = splicedHeadings.size();
            continue;
        }
        splicedHeadings << e;
    }
    if (insertAt < 0)
        insertAt = splicedHeadings.size();
    for (int i = 0; i < rescannedHeadings.size(); ++i)
        splicedHeadings.insert(insertAt + i, rescannedHeadings[i]);

    bool same = splicedHeadings.size() == m_headings.size();
    for (int i = 0; same && i < splicedHeadings.size(); ++i)
        same = splicedHeadings[i].block == m_headings[i].block && splicedHeadings[i].level == m_headings[i].level
            && splicedHeadings[i].text == m_headings[i].text;
    m_headings = splicedHeadings;
    if (!same)
        emit headingsChanged();

    // Splice m_blockCounts the same way (one pass, like m_headings above), adjusting the running
    // totals by the DELTA of only the blocks touched — the recount itself (the expensive part:
    // word-boundary analysis) only ever runs over [from, to], never the whole document (item 7).
    QList<BlockCount> splicedCounts;
    splicedCounts.reserve(m_blockCounts.size() + rescannedCounts.size());
    qint64 oldChars = 0, oldWords = 0;
    int countsInsertAt = -1;
    for (const BlockCount &e : std::as_const(m_blockCounts)) {
        const bool invalid = !e.block.isValid();
        const bool inSpan = !invalid && e.block.blockNumber() >= fromNum && e.block.blockNumber() <= toNum;
        if (invalid || inSpan) {
            oldChars += e.chars;
            oldWords += e.words;
            if (countsInsertAt < 0)
                countsInsertAt = splicedCounts.size();
            continue;
        }
        splicedCounts << e;
    }
    if (countsInsertAt < 0)
        countsInsertAt = splicedCounts.size();
    for (int i = 0; i < rescannedCounts.size(); ++i)
        splicedCounts.insert(countsInsertAt + i, rescannedCounts[i]);
    m_blockCounts = splicedCounts;
    m_totalChars += newChars - oldChars;
    m_totalWords += newWords - oldWords;
}

QVariantList RichTextHandler::headings() const
{
    QVariantList out;
    out.reserve(m_headings.size());
    for (const HeadingEntry &e : m_headings) {
        if (!e.block.isValid())
            continue; // defensive: handleContentsChange keeps this pruned
        out << QVariantMap {
            { QStringLiteral("position"), e.block.position() },
            { QStringLiteral("level"), e.block.blockFormat().headingLevel() },
            { QStringLiteral("text"), e.block.text() },
        };
    }
    return out;
}

int RichTextHandler::currentHeadingIndex() const
{
    const int pos = insertionCursor().position();
    int idx = -1;
    for (int i = 0; i < m_headings.size(); ++i) {
        if (!m_headings[i].block.isValid())
            continue;
        if (m_headings[i].block.position() > pos)
            break;
        idx = i;
    }
    return idx;
}

// ─── counts (office item 2b, item 7) ───────────────────────────────────────────────────────────
// wordCount()/charCount() are m_totalWords/m_totalChars directly (inline, in the header) — kept
// incrementally. The selection ones below are computed live: bounded by the selection's own
// size, not the document's, so there is nothing to keep incrementally.

int RichTextHandler::selectionWordCount() const
{
    const QTextCursor c = selectionCursor();
    return c.isNull() ? 0 : countWords(c.selection().toPlainText());
}

int RichTextHandler::selectionCharCount() const
{
    const QTextCursor c = selectionCursor();
    if (c.isNull())
        return 0;
    // Not toPlainText(): it turns BOTH the paragraph separator AND a soft line break (Shift+Enter,
    // U+2028) into "\n", which would also strip real line breaks from the count — charCount()
    // (whole-document) only excludes the paragraph separator (QTextBlock::text() already does).
    // One paragraph separator is crossed per block boundary inside the selection.
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return 0;
    const int blocksTouched = doc->findBlock(c.selectionEnd()).blockNumber()
        - doc->findBlock(c.selectionStart()).blockNumber() + 1;
    return (c.selectionEnd() - c.selectionStart()) - (blocksTouched - 1);
}

QString RichTextHandler::selectedText() const
{
    // Same convention as plainText(): paragraphs separated by "\n".
    const QTextCursor c = selectionCursor();
    return c.isNull() ? QString() : c.selection().toPlainText().replace(QChar::LineSeparator, QLatin1Char('\n'));
}

void RichTextHandler::setError(const QString &message)
{
    if (!message.isEmpty())
        emit ioError(message);
    if (m_error == message)
        return;
    m_error = message;
    emit errorStringChanged();
}

bool RichTextHandler::isModified() const
{
    const QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    return doc && doc->isModified();
}

void RichTextHandler::setModified(bool m)
{
    if (QTextDocument *doc = m_document ? m_document->textDocument() : nullptr)
        doc->setModified(m); // modificationChanged → modifiedChanged
}

QString RichTextHandler::alignment() const
{
    const QTextCursor c = insertionCursor();
    if (c.isNull())
        return QStringLiteral("left");
    const Qt::Alignment a = c.blockFormat().alignment();
    if (a & Qt::AlignHCenter) return QStringLiteral("center");
    if (a & Qt::AlignJustify) return QStringLiteral("justify");
    if (a & Qt::AlignRight) return QStringLiteral("right");
    return QStringLiteral("left");
}

void RichTextHandler::setAlignment(const QString &align)
{
    QTextCursor c = cursor();
    if (c.isNull())
        return;
    QTextBlockFormat bf;
    if (align == QLatin1String("center")) bf.setAlignment(Qt::AlignHCenter);
    else if (align == QLatin1String("right")) bf.setAlignment(Qt::AlignRight);
    else if (align == QLatin1String("justify")) bf.setAlignment(Qt::AlignJustify);
    else bf.setAlignment(Qt::AlignLeft);
    c.mergeBlockFormat(bf); // every block the selection touches
    emit formatChanged();
}

QVariantMap RichTextHandler::newDocument(const QVariantMap &options)
{
    const auto result = [](bool ok, const QString &error) {
        return QVariantMap { { QStringLiteral("ok"), ok }, { QStringLiteral("error"), error } };
    };
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return result(false, QStringLiteral("DocumentEditor: no document"));

    // Validate EVERYTHING first — on any error the document must be left exactly as it was.
    const QVariantMap stylesOpt = options.value(QStringLiteral("styles")).toMap();
    const QString paragraphStyleOpt = options.value(QStringLiteral("paragraphStyle")).toString();
    rt::StyleSheet sheet = rt::StyleSheet::builtIns();
    for (auto it = stylesOpt.cbegin(); it != stylesOpt.cend(); ++it) {
        rt::NamedStyle s;
        if (const QString err = buildStyle(sheet, it.key(), it.value().toMap(), &s); !err.isEmpty())
            return result(false, err);
        sheet.put(s);
    }
    QString firstStyle = rt::defaultParagraphStyle();
    if (!paragraphStyleOpt.isEmpty()) {
        const rt::NamedStyle *s = sheet.find(paragraphStyleOpt);
        if (!s || s->family != rt::Family::Paragraph)
            return result(false, QStringLiteral("unknown paragraph style \"%1\"").arg(paragraphStyleOpt));
        firstStyle = paragraphStyleOpt;
    }

    // doc->clear() invalidates every existing QTextBlock handle at once, as part of its OWN
    // contentsChange it fires while reinitializing — unlike a normal cursor edit, querying an
    // OLD handle still sitting in m_headings/m_blockCounts from inside that signal is a
    // use-after-free (observed crashing in QTextBlock::blockNumber(), reliably on a large
    // document). Disconnect handleContentsChange around it; rebuildIndex() below is a full,
    // safe rescan of the (now stable) fresh document anyway.
    QObject::disconnect(m_headingsConn);
    // Undo off: the reset (and styling the first paragraph) leaves no history.
    doc->setUndoRedoEnabled(false);
    doc->clear(); // also drops the image resources
    m_sheet = sheet;
    QTextCursor c(doc);
    c.setBlockFormat(m_sheet.blockFormat(firstStyle));
    c.setBlockCharFormat(m_sheet.charFormat(firstStyle, QString()));
    // m_sheet.blockFormat/charFormat() always materialize at zoom 1 (canonical units): bring the
    // fresh document up to the zoom already in effect (office item 2b, ★2), still with undo off.
    if (!qFuzzyCompare(m_zoom, 1.0))
        rescaleRange(-1, -1, m_zoom, false);
    doc->setUndoRedoEnabled(true);
    doc->setModified(false);
    m_headingsConn = connect(doc, &QTextDocument::contentsChange, this, &RichTextHandler::handleContentsChange);
    rebuildIndex(); // cheap full scan: infrequent (a new document), not a keystroke (★3)
    // clear() signals modificationChanged(true) and then resets the flag WITHOUT a signal, so
    // setModified(false) is a no-op: tell the bindings the flag is false again.
    emit modifiedChanged();
    setError(QString());
    emit stylesChanged();
    emit formatChanged();
    // wordCount/charCount's NOTIFY (office item 2b, item 7): doc->clear() above already relayed
    // its OWN contentsChanged through m_contentsConn, but that fired before rebuildIndex() ran,
    // so any listener (a QML binding) last saw the counts mid-reset, not the correct final ones
    // rebuildIndex() just computed — emit it again now that they're right.
    emit contentsChanged();
    return result(true, QString());
}

QString RichTextHandler::toHtml() const
{
    const QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    return doc ? doc->toHtml() : QString();
}

QString RichTextHandler::plainText() const
{
    const QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    // toPlainText turns paragraph separators into "\n"; line breaks (U+2028) stay — normalize them.
    return doc ? doc->toPlainText().replace(QChar::LineSeparator, QLatin1Char('\n')) : QString();
}

void RichTextHandler::insertText(const QString &text)
{
    QTextCursor c = insertionCursor();
    if (c.isNull())
        return;
    c.beginEditBlock();
    if (m_selectionStart != m_selectionEnd) {
        const int last = c.document()->characterCount() - 1;
        c.setPosition(qBound(0, m_selectionStart, last));
        c.setPosition(qBound(0, m_selectionEnd, last), QTextCursor::KeepAnchor); // typing over a selection replaces it
        c.removeSelectedText();
    }
    const QStringList paragraphs = text.split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < paragraphs.size(); ++i) {
        if (i > 0)
            breakParagraph(c);
        if (!paragraphs[i].isEmpty())
            c.insertText(paragraphs[i]);
    }
    c.endEditBlock();
    emit formatChanged();
}

// ─── clipboard (office item 2b, item 6) ───────────────────────────────────────────────────────

void RichTextHandler::copy() const
{
    const QTextCursor c = selectionCursor();
    if (c.isNull())
        return;
    const QTextDocumentFragment sel = c.selection();
    auto *data = new QMimeData;
    // Unscaled (office item 2b, ★2 follow-up): the clipboard always carries the document's
    // canonical (zoom-1) sizes, so a paste elsewhere — or back into this document at a different
    // zoom — reads at the original size instead of whatever this document's zoom happened to be.
    data->setHtml(unscaledHtml(sel));
    data->setText(sel.toPlainText());
    QGuiApplication::clipboard()->setMimeData(data);
}

void RichTextHandler::cut()
{
    QTextCursor c = selectionCursor();
    if (c.isNull())
        return;
    copy();
    c.removeSelectedText();
    emit formatChanged();
}

void RichTextHandler::paste()
{
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    if (!data || !(data->hasHtml() || data->hasText()))
        return;
    // HTML preferred (keeps formatting) and routed through insertHtml() — the SAME choke point
    // used for a direct insertHtml() call — so it is rescaled to the zoom in effect on entry
    // (office item 2b, ★2 follow-up); plain text has no size of its own, so insertText() just
    // takes the caret's own (already-zoomed) character format, like typed text.
    if (data->hasHtml())
        insertHtml(data->html());
    else
        insertText(data->text());
}

bool RichTextHandler::canPaste() const
{
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    return data && (data->hasHtml() || data->hasText());
}

void RichTextHandler::insertHtml(const QString &html)
{
    QTextCursor c = insertionCursor();
    if (c.isNull())
        return;
    c.beginEditBlock();
    if (const QTextCursor sel = selectionCursor(); !sel.isNull()) {
        c = sel;
        c.removeSelectedText();
    }
    const int from = c.position();
    c.insertHtml(html);
    // The HTML's own declared sizes (font-size, margins, …) are canonical, zoom-1 values — bring
    // just-inserted range up to the zoom in effect, same as typed text (office item 2b, ★2
    // follow-up). Content outside [from, to) (what the caret already sat in/next to) is untouched.
    if (!qFuzzyCompare(m_zoom, 1.0))
        rescaleInsertedRange(from, c.position(), m_zoom);
    c.endEditBlock();
    emit formatChanged();
}

void RichTextHandler::rescaleInsertedRange(int from, int to, qreal ratio)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc || qFuzzyCompare(ratio, 1.0) || to <= from)
        return;
    QTextCursor c(doc);
    const std::function<void(QTextFrame::iterator, const QTextFrame::iterator &)> walk =
        [&](QTextFrame::iterator it, const QTextFrame::iterator &end) {
            for (; it != end; ++it) {
                if (QTextFrame *f = it.currentFrame()) {
                    walk(f->begin(), f->end());
                    continue;
                }
                const QTextBlock b = it.currentBlock();
                if (!b.isValid())
                    continue;
                const int bpos = b.position(), bend = bpos + b.length();
                if (bend <= from || bpos >= to)
                    continue; // no overlap with the inserted range at all
                // Whether THIS block's own blockFormat/blockCharFormat just came off the pasted
                // source (canonical size — needs scaling) or is pre-existing (already correctly
                // zoomed — must NOT be touched) follows QTextCopyHelper::appendFragment's own
                // rule (qtextdocumentfragment.cpp), not a position heuristic: a paragraph
                // separator FROM THE PASTE (bpos > from: this block starts strictly inside the
                // inserted range, so Qt itself just created/restyled it off the pasted content —
                // true for every block after the first one touched, even when its tail end
                // absorbs pre-existing text after the paste, e.g. a 2-paragraph paste landing
                // before "Tail" leaves the SECOND pasted paragraph's format on "…Tail") ALWAYS
                // got a fresh format from the source. The one block the range merely starts
                // inside of (bpos <= from) keeps its own pre-existing format UNCHANGED — *unless*
                // the paste landed at the very start of the document (from == 0), the one case
                // where Qt's insertFragment special-cases position 0 and overwrites even that
                // block's format from the pasted source (verified against Qt 6.11 QTextGui).
                if (bpos > from || from == 0) {
                    c.setPosition(bpos);
                    c.setBlockFormat(rt::scaledLengths(b.blockFormat(), ratio));
                    c.setBlockCharFormat(rt::scaledLengths(b.charFormat(), ratio));
                }
                for (auto it2 = b.begin(); !it2.atEnd(); ++it2) {
                    const QTextFragment frag = it2.fragment();
                    if (!frag.isValid())
                        continue;
                    const int s = qMax(frag.position(), from);
                    const int e = qMin(frag.position() + frag.length(), to);
                    if (s >= e)
                        continue;
                    c.setPosition(s);
                    c.setPosition(e, QTextCursor::KeepAnchor);
                    c.setCharFormat(rt::scaledLengths(frag.charFormat(), ratio));
                }
            }
        };
    walk(doc->rootFrame()->begin(), doc->rootFrame()->end());
}

QString RichTextHandler::unscaledHtml(const QTextDocumentFragment &frag) const
{
    if (qFuzzyCompare(m_zoom, 1.0))
        return frag.toHtml();
    QTextDocument tmp;
    QTextCursor c(&tmp);
    c.insertFragment(frag);
    const qreal ratio = 1.0 / m_zoom;
    const std::function<void(QTextFrame::iterator, const QTextFrame::iterator &)> walk =
        [&](QTextFrame::iterator it, const QTextFrame::iterator &end) {
            for (; it != end; ++it) {
                if (QTextFrame *f = it.currentFrame()) {
                    walk(f->begin(), f->end());
                    continue;
                }
                const QTextBlock b = it.currentBlock();
                if (!b.isValid())
                    continue;
                c.setPosition(b.position());
                c.setBlockFormat(rt::scaledLengths(b.blockFormat(), ratio));
                c.setBlockCharFormat(rt::scaledLengths(b.charFormat(), ratio));
                for (auto it2 = b.begin(); !it2.atEnd(); ++it2) {
                    const QTextFragment f2 = it2.fragment();
                    if (!f2.isValid())
                        continue;
                    c.setPosition(f2.position());
                    c.setPosition(f2.position() + f2.length(), QTextCursor::KeepAnchor);
                    c.setCharFormat(rt::scaledLengths(f2.charFormat(), ratio));
                }
            }
        };
    walk(tmp.rootFrame()->begin(), tmp.rootFrame()->end());
    return tmp.toHtml();
}

void RichTextHandler::setSelectionStart(int v)
{
    if (m_selectionStart == v)
        return;
    m_selectionStart = v;
    emit selectionChanged();
    emit formatChanged();
}

void RichTextHandler::setSelectionEnd(int v)
{
    if (m_selectionEnd == v)
        return;
    m_selectionEnd = v;
    emit selectionChanged();
    emit formatChanged();
}

void RichTextHandler::setCursorPosition(int v)
{
    if (m_cursorPosition == v)
        return;
    m_cursorPosition = v;
    emit selectionChanged();
    emit formatChanged();
}

QTextCursor RichTextHandler::cursor() const
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return QTextCursor();
    QTextCursor c(doc);
    // The editor's positions reach us through bindings, a beat after the document changed under
    // them (a load, a new document): clamp so a stale value can't point past the end.
    const int last = doc->characterCount() - 1;
    if (m_selectionStart != m_selectionEnd) {
        c.setPosition(qBound(0, m_selectionStart, last));
        c.setPosition(qBound(0, m_selectionEnd, last), QTextCursor::KeepAnchor);
    } else {
        c.setPosition(qBound(0, m_cursorPosition, last));
        // Word-processor convention: with no selection, a format toggle applies to the word.
        c.select(QTextCursor::WordUnderCursor);
    }
    return c;
}

QTextCursor RichTextHandler::insertionCursor() const
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return QTextCursor();
    QTextCursor c(doc);
    c.setPosition(qBound(0, m_cursorPosition, doc->characterCount() - 1));
    return c;
}

void RichTextHandler::mergeCharFormat(const QTextCharFormat &fmt)
{
    QTextCursor c = cursor();
    if (c.isNull())
        return;
    c.mergeCharFormat(fmt);
    emit formatChanged();
}

bool RichTextHandler::bold() const { return cursor().charFormat().fontWeight() >= QFont::Bold; }
bool RichTextHandler::italic() const { return cursor().charFormat().fontItalic(); }
bool RichTextHandler::underline() const { return cursor().charFormat().fontUnderline(); }
bool RichTextHandler::strike() const { return cursor().charFormat().fontStrikeOut(); }

namespace {
bool isNumberedStyle(QTextListFormat::Style s)
{
    switch (s) {
    case QTextListFormat::ListDecimal: case QTextListFormat::ListLowerAlpha: case QTextListFormat::ListUpperAlpha:
    case QTextListFormat::ListLowerRoman: case QTextListFormat::ListUpperRoman:
        return true;
    default:
        return false;
    }
}
} // namespace

bool RichTextHandler::bulletList() const
{
    const QTextCursor c = cursor();
    const QTextList *l = c.isNull() ? nullptr : c.currentList();
    return l && !isNumberedStyle(l->format().style());
}

bool RichTextHandler::numberedList() const
{
    const QTextCursor c = cursor();
    const QTextList *l = c.isNull() ? nullptr : c.currentList();
    return l && isNumberedStyle(l->format().style());
}

int RichTextHandler::heading() const
{
    const QTextCursor c = cursor();
    return c.isNull() ? 0 : c.blockFormat().headingLevel();
}

QString RichTextHandler::fontFamily() const
{
    return rt::readProp(cursor().charFormat(), rt::Prop::FontFamily).toString();
}

qreal RichTextHandler::fontSize() const
{
    const QVariant v = rt::readProp(cursor().charFormat(), rt::Prop::FontSize);
    return v.isValid() ? v.toDouble() / m_zoom : 0.0; // unset: the editor's own base font stands in
}

void RichTextHandler::toggleBold()
{
    QTextCharFormat f;
    f.setFontWeight(bold() ? QFont::Normal : QFont::Bold);
    mergeCharFormat(f);
}

void RichTextHandler::toggleItalic()
{
    QTextCharFormat f;
    f.setFontItalic(!italic());
    mergeCharFormat(f);
}

void RichTextHandler::toggleUnderline()
{
    QTextCharFormat f;
    f.setFontUnderline(!underline());
    mergeCharFormat(f);
}

void RichTextHandler::toggleStrike()
{
    QTextCharFormat f;
    f.setFontStrikeOut(!strike());
    mergeCharFormat(f);
}

void RichTextHandler::toggleBulletList()
{
    QTextCursor c = cursor();
    if (c.isNull())
        return;
    if (QTextList *list = c.currentList()) {
        // Detach the block: reset its indentation back to body text. blockFormat() must be read
        // back AFTER remove() (not before) — remove() clears the block's own list association in
        // the document; a format snapshot taken earlier still carries the OLD one, and writing it
        // back would silently re-attach the block to the (possibly just-deleted) list.
        list->remove(c.block());
        QTextBlockFormat bf = c.blockFormat();
        bf.setIndent(0);
        c.setBlockFormat(bf);
    } else {
        c.createList(QTextListFormat::ListDisc);
    }
    emit formatChanged();
}

void RichTextHandler::toggleNumberedList()
{
    QTextCursor c = cursor();
    if (c.isNull())
        return;
    if (QTextList *list = c.currentList()) {
        // See toggleBulletList(): blockFormat() must be read back AFTER remove().
        list->remove(c.block());
        QTextBlockFormat bf = c.blockFormat();
        bf.setIndent(0);
        c.setBlockFormat(bf);
    } else {
        c.createList(QTextListFormat::ListDecimal);
    }
    emit formatChanged();
}

void RichTextHandler::setHeading(int level)
{
    applyParagraphStyle(level >= 1 && level <= 6 ? QStringLiteral("Heading %1").arg(level) : rt::textBodyStyle());
}

void RichTextHandler::setFontFamily(const QString &name)
{
    if (name.trimmed().isEmpty())
        return;
    QTextCharFormat f;
    f.setProperty(QTextFormat::FontFamilies, QStringList { name });
    mergeCharFormat(f);
}

void RichTextHandler::setFontSize(qreal pt)
{
    if (pt <= 0)
        return;
    QTextCharFormat f;
    f.setProperty(QTextFormat::FontPointSize, pt * m_zoom); // canonical in, baked in at the current zoom
    mergeCharFormat(f);
}

bool RichTextHandler::insertImage(const QUrl &file)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return false;
    const QString path = localPath(file);
    QImage img(path);
    if (img.isNull()) {
        setError(QStringLiteral("could not load image ") + path);
        return false;
    }
    // The resource name keys BOTH the render lookup and the ODF embed (the writer walks image
    // formats and pulls the resource into Pictures/).
    // A plain path becomes an absolute file:// URL: the lookup resolves names against the editor's
    // baseUrl, so a scheme-less key would never match again.
    const QUrl key = file.scheme().isEmpty() ? QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()) : file;
    doc->addResource(QTextDocument::ImageResource, key, img);
    QTextImageFormat fmt;
    fmt.setName(key.toString());
    // Word-like cap: images wider than the page column scale down, keeping aspect — at the zoom
    // in effect (★2), like every other length already in the document; the writer divides it back
    // out to the canonical 480px cap on save.
    const qreal maxWidth = 480.0 * m_zoom;
    if (img.width() > maxWidth) {
        fmt.setWidth(maxWidth);
        fmt.setHeight(img.height() * maxWidth / img.width());
    }
    insertionCursor().insertImage(fmt);
    setError(QString());
    emit formatChanged();
    return true;
}

void RichTextHandler::insertTable(int rows, int cols)
{
    QTextCursor c = insertionCursor();
    if (c.isNull() || rows < 1 || cols < 1)
        return;
    // Our ODF writer closes a list before a table, so a table may follow a list item directly.
    c.insertTable(rows, cols, rt::wordLikeTableFormat());
    emit formatChanged();
}

bool RichTextHandler::inTable() const
{
    const QTextCursor c = insertionCursor();
    return !c.isNull() && c.currentTable() != nullptr;
}

void RichTextHandler::addTableRow()
{
    QTextCursor c = insertionCursor();
    QTextTable *t = c.isNull() ? nullptr : c.currentTable();
    if (!t)
        return;
    t->insertRows(t->cellAt(c).row() + 1, 1);
    emit formatChanged();
}

void RichTextHandler::addTableColumn()
{
    QTextCursor c = insertionCursor();
    QTextTable *t = c.isNull() ? nullptr : c.currentTable();
    if (!t)
        return;
    t->insertColumns(t->cellAt(c).column() + 1, 1);
    emit formatChanged();
}

void RichTextHandler::removeTableRow()
{
    QTextCursor c = insertionCursor();
    QTextTable *t = c.isNull() ? nullptr : c.currentTable();
    if (!t)
        return;
    if (t->rows() <= 1)
        return; // removing the last row would leave a degenerate frame
    t->removeRows(t->cellAt(c).row(), 1);
    emit formatChanged();
}

void RichTextHandler::removeTableColumn()
{
    QTextCursor c = insertionCursor();
    QTextTable *t = c.isNull() ? nullptr : c.currentTable();
    if (!t)
        return;
    if (t->columns() <= 1)
        return;
    t->removeColumns(t->cellAt(c).column(), 1);
    emit formatChanged();
}

bool RichTextHandler::writeOdf(const QUrl &file)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return false;
    // Our writer (not QTextDocumentWriter's "odf"): named styles in styles.xml, headings as text:h.
    // It reads back the live (possibly zoomed) formats in canonical units itself (★2) — m_sheet
    // stays the unscaled source of truth either way.
    QString why;
    if (!rt::writeOdt(localPath(file), doc, m_sheet, m_zoom, &why)) {
        setError(why);
        return false;
    }
    setError(QString());
    return true;
}

bool RichTextHandler::saveOdf(const QUrl &file)
{
    if (!writeOdf(file))
        return false;
    m_document->textDocument()->setModified(false);
    return true;
}

bool RichTextHandler::saveOdfCopy(const QUrl &file)
{
    return writeOdf(file);
}

bool RichTextHandler::loadOdf(const QUrl &file)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return false;
    QString why;
    rt::StyleSheet sheet;
    // readOdt() calls doc->clear() internally when it does touch the document — see newDocument()
    // for why handleContentsChange must not run across that (a use-after-free on old QTextBlock
    // handles still in m_headings/m_blockCounts).
    QObject::disconnect(m_headingsConn);
    const bool ok = rt::readOdt(localPath(file), doc, &sheet, &why);
    m_headingsConn = connect(doc, &QTextDocument::contentsChange, this, &RichTextHandler::handleContentsChange);
    if (!ok && sheet.isEmpty()) { // not found / not an .odt: the document was not touched
        setError(why);
        return false;
    }
    m_sheet = sheet;
    // readOdt materialized at zoom 1 (canonical units): bring the document up to the zoom already
    // in effect (office item 2b, ★2). Undo is briefly disabled around it, as readOdt's own build
    // already was, so this still reads as "the load is not an undo step".
    if (!qFuzzyCompare(m_zoom, 1.0)) {
        doc->setUndoRedoEnabled(false);
        rescaleRange(-1, -1, m_zoom, false);
        doc->setUndoRedoEnabled(true);
        doc->setModified(false);
    }
    rebuildIndex(); // cheap full scan: infrequent (a load), not a keystroke (★3)
    emit modifiedChanged(); // see newDocument: clear() can leave the last signal saying "true"
    emit stylesChanged();
    setError(why);
    emit formatChanged();
    // wordCount/charCount's NOTIFY (office item 2b, item 7): readOdt()'s own edits already
    // relayed contentsChanged through m_contentsConn repeatedly WHILE building the document, all
    // of it before rebuildIndex() ran just above — a listener (a QML binding) last saw whatever
    // the counts were mid-build (0, or the PREVIOUS document's counts), never the correct final
    // ones. Emit it again now that they're right.
    emit contentsChanged();
    return ok;
}

// ─── named styles ──────────────────────────────────────────────────────────────────────────────

// A style definition change inside the document's undo history: undoing the re-styled paragraphs
// also puts the old definition back (and redo the new one).
class StyleUndo : public QAbstractUndoItem {
public:
    StyleUndo(RichTextHandler *h, const QString &name, const std::optional<rt::NamedStyle> &before,
              const rt::NamedStyle &after)
        : m_handler(h), m_name(name), m_before(before), m_after(after) {}
    void undo() override { if (m_handler) m_handler->restoreStyle(m_name, m_before); }
    void redo() override { if (m_handler) m_handler->restoreStyle(m_name, m_after); }

private:
    QPointer<RichTextHandler> m_handler;
    QString m_name;
    std::optional<rt::NamedStyle> m_before;
    rt::NamedStyle m_after;
};

void RichTextHandler::restoreStyle(const QString &name, const std::optional<rt::NamedStyle> &style)
{
    if (style)
        m_sheet.put(*style);
    else
        m_sheet.remove(name);
    // Called from inside QTextDocument::undo/redo: tell the bindings once the document is whole.
    QMetaObject::invokeMethod(this, &RichTextHandler::stylesChanged, Qt::QueuedConnection);
    QMetaObject::invokeMethod(this, &RichTextHandler::formatChanged, Qt::QueuedConnection);
}

namespace {
// Builds and validates ONE style definition against `sheet` (read-only here): "" and *out set to
// the resulting NamedStyle, or the error (sheet/*out untouched) — shared by defineStyle() (against
// m_sheet, with the live-document/undo side effects below) and newDocument(options) (office item
// 2b addendum: against a scratch sheet, before any document exists to restyle).
QString buildStyle(const rt::StyleSheet &sheet, const QString &name, const QVariantMap &props, rt::NamedStyle *out)
{
    const rt::NamedStyle *old = sheet.find(name);
    rt::NamedStyle s;
    if (old) {
        s = *old;
    } else {
        s.name = name;
        const QString family = props.value(QStringLiteral("family"), QStringLiteral("paragraph")).toString();
        if (family == QLatin1String("character") || family == QLatin1String("text"))
            s.family = rt::Family::Character;
        else if (family != QLatin1String("paragraph"))
            return QStringLiteral("family: expected \"paragraph\" or \"character\"");
    }
    for (auto it = props.cbegin(); it != props.cend(); ++it) {
        const QString &key = it.key();
        const QVariant &v = it.value();
        const bool unset = !v.isValid() || v.isNull();
        if (key == QLatin1String("family")) {
            const QString f = v.toString();
            const bool character = f == QLatin1String("character") || f == QLatin1String("text");
            if (old && character != (s.family == rt::Family::Character))
                return QStringLiteral("\"%1\" is a %2 style").arg(name, s.family == rt::Family::Character
                                                                    ? QStringLiteral("character") : QStringLiteral("paragraph"));
        } else if (key == QLatin1String("parent")) {
            s.parent = unset ? QString() : v.toString();
        } else if (key == QLatin1String("nextStyle")) {
            s.next = unset ? QString() : v.toString();
        } else {
            rt::Prop p;
            if (!rt::propFromName(key, &p))
                return QStringLiteral("unknown style property \"%1\"").arg(key);
            if (s.family == rt::Family::Character && !rt::isCharProp(p))
                return QStringLiteral("%1 is a paragraph property (\"%2\" is a character style)").arg(key, name);
            if (unset) {
                s.props.remove(key);
            } else {
                QVariant canonical;
                QString why;
                if (!rt::canonicalValue(p, v, &canonical, &why))
                    return why;
                s.props.insert(key, canonical);
            }
        }
    }
    if (const QString why = sheet.validate(s); !why.isEmpty())
        return why;
    *out = s;
    return QString();
}
} // namespace

QString RichTextHandler::defineStyle(const QString &name, const QVariantMap &props)
{
    rt::NamedStyle s;
    if (const QString err = buildStyle(m_sheet, name, props, &s); !err.isEmpty())
        return err;
    const rt::NamedStyle *old = m_sheet.find(name);
    if (old && old->parent == s.parent && old->next == s.next && old->props == s.props)
        return QString(); // nothing changes

    const rt::StyleSheet before = m_sheet;
    m_sheet.put(s);
    if (QTextDocument *doc = m_document ? m_document->textDocument() : nullptr) {
        QTextCursor edit(doc);
        edit.beginEditBlock();
        if (doc->isUndoRedoEnabled())
            doc->appendUndoItem(new StyleUndo(this, name, old ? std::optional<rt::NamedStyle>(*old) : std::nullopt,
                                              *m_sheet.find(name)));
        const auto same = [](const QString &n) { return n; };
        restyle(-1, -1, before, same, same, false, false);
        edit.endEditBlock();
        doc->setModified(true); // a definition is part of the document even when no text uses it
    }
    emit stylesChanged();
    emit contentsChanged();
    emit formatChanged();
    return QString();
}

QStringList RichTextHandler::styleNames(const QString &family) const
{
    if (family == QLatin1String("character") || family == QLatin1String("text"))
        return m_sheet.names(rt::Family::Character);
    if (family == QLatin1String("paragraph"))
        return m_sheet.names(rt::Family::Paragraph);
    return {};
}

QVariantMap RichTextHandler::styleProperties(const QString &name, bool resolved) const
{
    const rt::NamedStyle *s = m_sheet.find(name);
    if (!s)
        return {};
    QVariantMap out = resolved ? m_sheet.resolved(name) : s->props;
    const bool paragraph = s->family == rt::Family::Paragraph;
    out.insert(QStringLiteral("family"), paragraph ? QStringLiteral("paragraph") : QStringLiteral("character"));
    out.insert(QStringLiteral("parent"), s->parent);
    if (paragraph)
        out.insert(QStringLiteral("nextStyle"), s->next.isEmpty() ? s->name : s->next);
    return out;
}

void RichTextHandler::restyle(int from, int to, const rt::StyleSheet &oldSheet,
                              const std::function<QString(const QString &)> &paraMap,
                              const std::function<QString(const QString &)> &charMap, bool force, bool clipChars)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return;
    const int last = doc->characterCount() - 1;
    QTextBlock b = from < 0 ? doc->begin() : doc->findBlock(qBound(0, from, last));
    const QTextBlock endBlock = from < 0 ? QTextBlock() : doc->findBlock(qBound(0, to, last));
    QTextCursor c(doc);
    struct Change { int pos; int len; QTextCharFormat format; };
    QList<Change> changes;
    for (; b.isValid(); b = b.next()) {
        const QTextBlockFormat bf = b.blockFormat();
        const QString oldPara = rt::paragraphStyleOf(bf);
        const QString newPara = paraMap(oldPara);
        const QTextBlockFormat oldBase = oldSheet.blockFormat(oldPara);
        const QTextBlockFormat newBase = m_sheet.blockFormat(newPara);
        if (force || oldBase != newBase) {
            QTextBlockFormat nbf = bf;
            rt::rebase(nbf, oldBase, newBase, true, force);
            nbf.setProperty(rt::kParagraphStyleProperty, newPara);
            if (nbf != bf) {
                c.setPosition(b.position());
                c.setBlockFormat(nbf);
            }
        }
        const auto restyleChars = [&](const QTextCharFormat &f, QTextCharFormat *out) {
            const QString oldCs = rt::charStyleOf(f);
            const QString newCs = charMap(oldCs);
            const QTextCharFormat oldB = oldSheet.charFormat(oldPara, oldCs);
            const QTextCharFormat newB = m_sheet.charFormat(newPara, newCs);
            if (!force && oldB == newB)
                return false;
            *out = f;
            rt::rebase(*out, oldB, newB, false, force);
            if (newCs.isEmpty()) out->clearProperty(rt::kCharStyleProperty);
            else out->setProperty(rt::kCharStyleProperty, newCs);
            return *out != f;
        };
        if (!clipChars) { // what is typed into the paragraph when it is empty
            const QTextCharFormat bcf = b.charFormat();
            QTextCharFormat nbcf;
            if (restyleChars(bcf, &nbcf)) {
                c.setPosition(b.position());
                c.setBlockCharFormat(nbcf);
            }
        }
        changes.clear();
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid() || frag.charFormat().isImageFormat())
                continue;
            int s = frag.position(), e = frag.position() + frag.length();
            if (clipChars) {
                s = qMax(s, from);
                e = qMin(e, to);
                if (s >= e)
                    continue;
            }
            QTextCharFormat nf;
            if (restyleChars(frag.charFormat(), &nf))
                changes << Change { s, e - s, nf };
        }
        for (const Change &ch : std::as_const(changes)) {
            c.setPosition(ch.pos);
            c.setPosition(ch.pos + ch.len, QTextCursor::KeepAnchor);
            c.setCharFormat(ch.format);
        }
        if (from >= 0 && b == endBlock)
            break;
    }
    // rebase() above always takes raw values from m_sheet (canonical, zoom-1 units): bring what
    // was just touched back up to the zoom in effect (office item 2b, ★2). Every restyle() caller
    // already runs inside its own edit block, so this stays part of the same undo step. Mirrors
    // clipChars exactly: a whole-block operation (false) rescales every run of the touched
    // block(s); a selection-scoped one (true) only the runs inside [from, to] — the others were
    // never touched by rebase() above and are already at the right zoom.
    if (!qFuzzyCompare(m_zoom, 1.0))
        rescaleRange(from, to, m_zoom, clipChars);
}

void RichTextHandler::rescaleRange(int from, int to, qreal ratio, bool clipChars)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc || qFuzzyCompare(ratio, 1.0))
        return;
    QTextCursor c(doc);
    const auto scaleBlock = [&](const QTextBlock &b) {
        if (from >= 0 && (b.position() + b.length() <= from || b.position() > to))
            return;
        c.setPosition(b.position());
        c.setBlockFormat(rt::scaledLengths(b.blockFormat(), ratio));
        if (!clipChars)
            c.setBlockCharFormat(rt::scaledLengths(b.charFormat(), ratio));
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid())
                continue;
            int s = frag.position(), e = s + frag.length();
            if (clipChars) {
                s = qMax(s, from);
                e = qMin(e, to);
                if (s >= e)
                    continue;
            }
            c.setPosition(s);
            c.setPosition(e, QTextCursor::KeepAnchor);
            c.setCharFormat(rt::scaledLengths(frag.charFormat(), ratio));
        }
    };
    const std::function<void(QTextFrame::iterator, const QTextFrame::iterator &)> walk =
        [&](QTextFrame::iterator it, const QTextFrame::iterator &end) {
            for (; it != end; ++it) {
                if (QTextFrame *f = it.currentFrame())
                    walk(f->begin(), f->end());
                else if (const QTextBlock b = it.currentBlock(); b.isValid())
                    scaleBlock(b);
            }
        };
    walk(doc->rootFrame()->begin(), doc->rootFrame()->end());
}

void RichTextHandler::setZoom(qreal z)
{
    z = qBound(0.5, z, 3.0);
    if (qFuzzyCompare(z, m_zoom))
        return;
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    const qreal ratio = z / m_zoom;
    if (doc) {
        QTextCursor edit(doc);
        edit.beginEditBlock(); // one undo step, like any other formatting change
        rescaleRange(-1, -1, ratio, false);
        edit.endEditBlock();
    }
    m_zoom = z;
    emit zoomChanged();
}

QTextCursor RichTextHandler::selectionCursor() const
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc || m_selectionStart == m_selectionEnd)
        return QTextCursor();
    const int last = doc->characterCount() - 1;
    QTextCursor c(doc);
    c.setPosition(qBound(0, m_selectionStart, last));
    c.setPosition(qBound(0, m_selectionEnd, last), QTextCursor::KeepAnchor);
    return c;
}

bool RichTextHandler::applyParagraphStyle(const QString &name)
{
    const rt::NamedStyle *s = m_sheet.find(name);
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!s || s->family != rt::Family::Paragraph || !doc)
        return false;
    const QTextCursor sel = selectionCursor();
    const QTextCursor at = insertionCursor();
    const int from = sel.isNull() ? at.position() : sel.selectionStart();
    const int to = sel.isNull() ? at.position() : sel.selectionEnd();
    QTextCursor edit(doc);
    edit.beginEditBlock();
    restyle(from, to, m_sheet, [&](const QString &) { return name; }, [](const QString &n) { return n; }, false, false);
    edit.endEditBlock();
    emit formatChanged();
    return true;
}

bool RichTextHandler::applyCharStyle(const QString &name)
{
    const rt::NamedStyle *s = name.isEmpty() ? nullptr : m_sheet.find(name);
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if ((!name.isEmpty() && (!s || s->family != rt::Family::Character)) || !doc)
        return false;
    const QTextCursor c = cursor(); // the selection, or the word at the caret
    if (c.hasSelection()) {
        QTextCursor edit(doc);
        edit.beginEditBlock();
        restyle(c.selectionStart(), c.selectionEnd(), m_sheet, [](const QString &n) { return n; },
                [&](const QString &) { return name; }, false, true);
        edit.endEditBlock();
    }
    emit formatChanged();
    return true;
}

QString RichTextHandler::currentParagraphStyle() const
{
    const QTextCursor c = insertionCursor();
    return c.isNull() ? rt::defaultParagraphStyle() : rt::paragraphStyleOf(c.block());
}

QString RichTextHandler::currentCharStyle() const
{
    const QTextCursor c = cursor();
    return c.isNull() ? QString() : rt::charStyleOf(c.charFormat());
}

void RichTextHandler::clearDirectFormatting()
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return;
    const QTextCursor sel = selectionCursor();
    const QTextBlock block = insertionCursor().block();
    const int from = sel.isNull() ? block.position() : sel.selectionStart();
    const int to = sel.isNull() ? block.position() + block.length() - 1 : sel.selectionEnd();
    const auto same = [](const QString &n) { return n; };
    QTextCursor edit(doc);
    edit.beginEditBlock();
    restyle(from, to, m_sheet, same, same, true, !sel.isNull());
    edit.endEditBlock();
    emit formatChanged();
}

void RichTextHandler::clearFormatting()
{
    // Office item 2b (★4): character properties only — restyle()'s `force` can't be split into
    // block/char halves, so this walks the runs itself and replaces each one with exactly what
    // its (kept) character style gives, leaving the block/paragraph format untouched.
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return;
    const QTextCursor sel = selectionCursor();
    const QTextBlock block = insertionCursor().block();
    const int from = sel.isNull() ? block.position() : sel.selectionStart();
    const int to = sel.isNull() ? block.position() + block.length() - 1 : sel.selectionEnd();
    const int last = doc->characterCount() - 1;
    QTextCursor edit(doc);
    edit.beginEditBlock();
    QTextCursor c(doc);
    QTextBlock b = doc->findBlock(qBound(0, from, last));
    const QTextBlock endBlock = doc->findBlock(qBound(0, to, last));
    for (; b.isValid(); b = b.next()) {
        const QString para = rt::paragraphStyleOf(b);
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid() || frag.charFormat().isImageFormat())
                continue;
            const int s = qMax(frag.position(), from);
            const int e = qMin(frag.position() + frag.length(), to);
            if (s >= e)
                continue;
            const QString cs = rt::charStyleOf(frag.charFormat());
            c.setPosition(s);
            c.setPosition(e, QTextCursor::KeepAnchor);
            c.setCharFormat(m_sheet.charFormat(para, cs)); // the style chain exactly — keeps cs, drops direct overrides
        }
        if (b == endBlock)
            break;
    }
    edit.endEditBlock();
    // m_sheet.charFormat() above materializes at zoom 1 (canonical units), like restyle() does.
    if (!qFuzzyCompare(m_zoom, 1.0))
        rescaleRange(from, to, m_zoom, true);
    emit formatChanged();
}

QVariantMap RichTextHandler::directFormattingAt(int pos) const
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return {};
    pos = qBound(0, pos, doc->characterCount() - 1);
    const QTextBlock b = doc->findBlock(pos);
    const QString para = rt::paragraphStyleOf(b);
    QTextCursor c(doc);
    // charFormat() is the format of the character BEFORE the cursor: step over the one at pos.
    c.setPosition(pos < b.position() + b.length() - 1 ? pos + 1 : pos);
    const QTextCharFormat f = c.charFormat();
    const QString cs = rt::charStyleOf(f);
    return {
        { QStringLiteral("paragraphStyle"), para },
        { QStringLiteral("charStyle"), cs },
        { QStringLiteral("paragraph"), rt::directProps(b.blockFormat(), m_sheet.blockFormat(para), true) },
        { QStringLiteral("character"), rt::directProps(f, m_sheet.charFormat(para, cs), false) },
    };
}

void RichTextHandler::breakParagraph(QTextCursor &c)
{
    const QTextBlock b = c.block();
    const bool atEnd = c.atBlockEnd();
    const QString style = rt::paragraphStyleOf(b);
    QString next = style;
    if (const rt::NamedStyle *s = m_sheet.find(style); s && !s->next.isEmpty() && m_sheet.find(s->next))
        next = s->next;
    c.insertBlock(); // the new paragraph starts as a copy of this one (style, list, direct formatting)
    if (atEnd && next != style) {
        const int pos = c.position();
        restyle(pos, pos, m_sheet, [&](const QString &) { return next; }, [](const QString &n) { return n; }, false, false);
        c.setCharFormat(c.block().charFormat()); // type in the new style
    }
}

void RichTextHandler::insertParagraph()
{
    QTextCursor c = insertionCursor();
    if (c.isNull())
        return;
    c.beginEditBlock();
    if (const QTextCursor sel = selectionCursor(); !sel.isNull()) {
        c = sel;
        c.removeSelectedText();
    }
    breakParagraph(c);
    c.endEditBlock();
    emit formatChanged();
}

// ─── paragraph (office item 2b, item 5) ───────────────────────────────────────────────────────

void RichTextHandler::indent()
{
    // 24px (~0.25in), the common word-processor step; at the zoom in effect, like setFontSize().
    constexpr qreal kIndentStep = 24.0;
    QTextCursor c = cursor();
    if (c.isNull())
        return;
    if (QTextList *list = c.currentList()) {
        QTextListFormat nf = list->format();
        nf.setIndent(nf.indent() + 1);
        list->remove(c.block());
        c.createList(nf); // a new (or implicitly shared, by Qt) QTextList at the deeper level
    } else {
        QTextBlockFormat bf = c.blockFormat();
        bf.setLeftMargin(bf.leftMargin() + kIndentStep * m_zoom);
        c.setBlockFormat(bf);
    }
    emit formatChanged();
}

void RichTextHandler::outdent()
{
    constexpr qreal kIndentStep = 24.0;
    QTextCursor c = cursor();
    if (c.isNull())
        return;
    if (QTextList *list = c.currentList()) {
        QTextListFormat nf = list->format();
        const int newIndent = nf.indent() - 1;
        list->remove(c.block()); // see toggleBulletList(): blockFormat() must be read back AFTER this
        if (newIndent <= 0) {
            QTextBlockFormat bf = c.blockFormat();
            bf.setIndent(0);
            c.setBlockFormat(bf);
        } else {
            nf.setIndent(newIndent);
            c.createList(nf);
        }
    } else {
        QTextBlockFormat bf = c.blockFormat();
        bf.setLeftMargin(qMax(0.0, bf.leftMargin() - kIndentStep * m_zoom));
        c.setBlockFormat(bf);
    }
    emit formatChanged();
}

void RichTextHandler::insertPageBreak()
{
    QTextCursor c = insertionCursor();
    if (c.isNull())
        return;
    c.beginEditBlock();
    if (const QTextCursor sel = selectionCursor(); !sel.isNull()) {
        c = sel;
        c.removeSelectedText();
    }
    breakParagraph(c);
    QTextBlockFormat bf = c.blockFormat();
    bf.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore);
    c.setBlockFormat(bf);
    c.endEditBlock();
    emit formatChanged();
}
