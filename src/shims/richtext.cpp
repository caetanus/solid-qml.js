#include "richtext.h"
#include "richtext_odf.h"

#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPromise>
#include <QPixmap>
#include <QThread>
#include <QThreadPool>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextBoundaryFinder>
#include <QVarLengthArray>
#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QPainterPath>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGRectangleNode>
#include <QSGTexture>
#include <QTextLayout>
#include <QtMath>
#include <algorithm>
#include <limits>
#include <QTextDocumentFragment>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextImageFormat>
#include <QTextList>
#include <QTextTable>
#include <QAbstractUndoItem>
#include <QPointer>
#include <private/qquicktextedit_p.h>
#include <private/qquicktextedit_p_p.h>
#include <private/qquicktextcontrol_p.h>
#include <private/qquicktextcontrol_p_p.h>

class MarksOverlay; // the marks overlay item (defined with the marks, below)
namespace {
QQuickItem *createMarksOverlay(RichTextHandler *handler, QQuickItem *editor);
QQuickItem *createPageBreakOverlay(RichTextHandler *handler, QQuickItem *editor);
}

RichTextHandler::RichTextHandler(QObject *parent) : QObject(parent)
{
    // The system clipboard, not this document: canPaste must track another app copying something
    // too (office item 2b, item 6).
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &RichTextHandler::canPasteChanged);
}

RichTextHandler::~RichTextHandler()
{
    // The document may outlive us (its blocks' BlockInfo must not report to a dead handler).
    detachBlockInfos();
    delete m_overlay.data();
    delete m_pageBreakOverlay.data();
}

rt::BlockInfo::~BlockInfo()
{
    if (owner)
        owner->blockInfoDestroyed(this);
}

void RichTextHandler::setDocument(QQuickTextDocument *doc)
{
    if (m_document == doc)
        return;
    QObject::disconnect(m_modifiedConn);
    QObject::disconnect(m_contentsConn);
    QObject::disconnect(m_cullingConn);
    QObject::disconnect(m_headingsConn);
    QObject::disconnect(m_undoAddedConn);
    detachBlockInfos();
    delete m_overlay.data();
    delete m_pageBreakOverlay.data();
    m_pendingFont = {};
    m_document = doc;
    m_doc = doc ? doc->textDocument() : nullptr;
    // The marks overlay: a child of the editor (QQuickTextEdit parents its QQuickTextDocument).
    if (auto *editor = doc ? qobject_cast<QQuickItem *>(doc->parent()) : nullptr) {
        m_overlay = createMarksOverlay(this, editor);
        m_pageBreakOverlay = createPageBreakOverlay(this, editor);
    }
    if (QTextDocument *td = doc ? doc->textDocument() : nullptr) {
        // Office item 3: emitted (before contentsChange) for NEW edits only — how trackBlocks()
        // tells an edit from an undo/redo replaying the stack.
        m_undoAddedConn = connect(td, &QTextDocument::undoCommandAdded, this, [this] { m_newUndoCommand = true; });
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
    // rebuildBlockIds() first: rebuildIndex() keys every heading/count entry by blockId, so every
    // block must already carry a BlockInfo/id by the time it runs.
    m_pendingFont = {};
    rebuildBlockIds();
    rebuildIndex(); // a full, but infrequent (document-attach-time) scan establishes the baseline
    emit documentChanged();
    emit formatChanged();
    emit modifiedChanged();
    // Same reasoning as loadOdf()/newDocument(): rebuildIndex() just computed wordCount/charCount
    // for the newly-attached document — tell any binding on them (office item 2b, item 7).
    emit contentsChanged();
    emit blocksLoaded();
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

void RichTextHandler::refreshViewport()
{
    auto *edit = m_document ? qobject_cast<QQuickTextEdit *>(m_document->parent()) : nullptr;
    if (!edit || !edit->flags().testFlag(QQuickItem::ItemObservesViewport))
        return;
    // Qt 6.11's backwards node traversal stops at a nested frame (a table), because
    // that iterator entry has no currentBlock().layout(). It can leave the first
    // rendered block BELOW the viewport. Restart from the root on backward scroll;
    // culling stays enabled, so only visible nodes are rebuilt, without any relayout.
    auto *d = static_cast<QQuickTextEditPrivate *>(QQuickItemPrivate::get(edit));
    const QRectF viewport = edit->clipRect();
    if (viewport.top() > d->renderedRegion.top() && viewport.bottom() < d->renderedRegion.bottom())
        return; // the existing nodes still cover this small backwards movement
    d->updateType = QQuickTextEditPrivate::UpdateAll;
    // UpdateAll drops the nodes, but Qt otherwise retains their old coverage. Clear
    // it too, or a subsequent forward scroll can be mistaken for already-painted text.
    d->renderedRegion = QRectF();
    edit->polish();
    edit->update();
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
    // Requires rebuildBlockIds() to have already run (setDocument/newDocument/loadOdf all call it
    // first): every block below must already carry a BlockInfo/id to be keyed by.
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    const QList<HeadingEntry> before = m_headings;
    m_headings.clear();
    m_pageBreaks.clear();
    m_blockCounts.clear();
    m_totalChars = 0;
    m_totalWords = 0;
    if (doc) {
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            const rt::BlockInfo *info = rt::blockInfo(b);
            const QString id = info ? info->id : QString();
            const QTextBlockFormat format = b.blockFormat();
            if (format.pageBreakPolicy().testFlag(QTextFormat::PageBreak_AlwaysBefore))
                m_pageBreaks.insert(id);
            const QString text = b.text();
            const int chars = text.size();
            const int words = countWords(text);
            m_blockCounts << BlockCount { id, chars, words };
            m_totalChars += chars;
            m_totalWords += words;
            const int level = format.headingLevel();
            if (level > 0)
                m_headings << HeadingEntry { id, level, text };
        }
    }
    bool same = before.size() == m_headings.size();
    for (int i = 0; same && i < before.size(); ++i)
        same = before[i].blockId == m_headings[i].blockId && before[i].level == m_headings[i].level
            && before[i].text == m_headings[i].text;
    if (!same)
        emit headingsChanged();
}

void RichTextHandler::handleContentsChange(int position, int charsRemoved, int charsAdded)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    // A load/new/clear is about to rebuild (or just rebuilt) the WHOLE document at once: skip both
    // trackBlocks() (which already no-ops under m_bulkReset on its own) and the headings/counts
    // rescan+splice below for every intermediate contentsChange a bulk rebuild fires — rebuildBlockIds()/
    // rebuildIndex(), called once after the bulk op finishes, fully recompute everything instead,
    // which is cheaper (no incremental upkeep repeated across potentially thousands of edits) and
    // safe (no liveness bookkeeping needs to run while the document is mid-reinitialization; see
    // blockInfoDestroyed()). This is what makes newDocument()/loadOdf() safe to run doc->clear()
    // (and readOdt()'s own many edits) with handleContentsChange left connected throughout.
    if (!doc || m_bulkReset)
        return;
    const int last = doc->characterCount() - 1;
    const QTextBlock from = doc->findBlock(qBound(0, position, last));
    const QTextBlock to = doc->findBlock(qBound(0, position + charsAdded, last));
    const int fromNum = from.blockNumber(), toNum = to.blockNumber();
    trackBlocks(position, charsRemoved, charsAdded); // office item 3: ids, text changes — also the
        // point after which every block of [from, to] is guaranteed a BlockInfo/id (fresh blocks
        // get one there), which the rescan walk below relies on.

    // What the re-scanned span gives NOW (document order, since we walk from→to forward) — both
    // the headings in it and every block's counts (item 7), in the SAME walk.
    QList<HeadingEntry> rescannedHeadings;
    QList<BlockCount> rescannedCounts;
    qint64 newChars = 0, newWords = 0;
    for (QTextBlock b = from; b.isValid(); b = b.next()) {
        const rt::BlockInfo *info = rt::blockInfo(b);
        const QString id = info ? info->id : QString();
        const QTextBlockFormat format = b.blockFormat();
        if (format.pageBreakPolicy().testFlag(QTextFormat::PageBreak_AlwaysBefore))
            m_pageBreaks.insert(id);
        else
            m_pageBreaks.remove(id);
        const QString text = b.text();
        const int chars = text.size();
        const int words = countWords(text);
        rescannedCounts << BlockCount { id, chars, words };
        newChars += chars;
        newWords += words;
        const int level = format.headingLevel();
        if (level > 0)
            rescannedHeadings << HeadingEntry { id, level, text };
        if (b == to)
            break;
    }

    // Splice m_headings: keep every OLD heading outside [from, to] and still alive; everything
    // inside that span — dead or not — is replaced by `rescannedHeadings`, inserted where the
    // span's old entries were (they are contiguous, since m_headings is kept in document order,
    // and every block that died in THIS edit was, before it, somewhere inside the old
    // [position, position + charsRemoved) range the edit replaced).
    //
    // "Alive" is a m_blocksById lookup — NEVER a stale QTextBlock handle. blockInfoDestroyed()
    // (the ~BlockInfo hook) removes a dying block's id from m_blocksById SYNCHRONOUSLY, as part of
    // the document edit itself, strictly before this slot runs; resolving a LIVE entry's current
    // blockNumber() through that same live handle is safe and behaves exactly as the entry's own
    // (no longer stored) handle would have.
    QList<HeadingEntry> splicedHeadings;
    splicedHeadings.reserve(m_headings.size() + rescannedHeadings.size());
    int insertAt = -1;
    for (const HeadingEntry &e : std::as_const(m_headings)) {
        const auto it = m_blocksById.constFind(e.blockId);
        const bool dead = it == m_blocksById.cend();
        const bool inSpan = !dead && it->blockNumber() >= fromNum && it->blockNumber() <= toNum;
        if (dead || inSpan) {
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
        same = splicedHeadings[i].blockId == m_headings[i].blockId && splicedHeadings[i].level == m_headings[i].level
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
        const auto it = m_blocksById.constFind(e.blockId);
        const bool dead = it == m_blocksById.cend();
        const bool inSpan = !dead && it->blockNumber() >= fromNum && it->blockNumber() <= toNum;
        if (dead || inSpan) {
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
        const auto it = m_blocksById.constFind(e.blockId);
        if (it == m_blocksById.cend())
            continue; // defensive: handleContentsChange keeps this pruned
        out << QVariantMap {
            { QStringLiteral("position"), it->position() },
            { QStringLiteral("level"), it->blockFormat().headingLevel() },
            { QStringLiteral("text"), it->text() },
            { QStringLiteral("blockId"), e.blockId }, // stable across edits: a <For key="blockId">
        };
    }
    return out;
}

int RichTextHandler::currentHeadingIndex() const
{
    const int pos = insertionCursor().position();
    int idx = -1;
    for (int i = 0; i < m_headings.size(); ++i) {
        const auto it = m_blocksById.constFind(m_headings[i].blockId);
        if (it == m_blocksById.cend())
            continue;
        if (it->position() > pos)
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

    // doc->clear() fires its OWN contentsChange (handleContentsChange runs, same as for any other
    // edit) WHILE reinitializing. That used to be unsafe (an old QTextBlock handle in m_headings/
    // m_blockCounts, read after clear() freed its block, is a use-after-free: observed crashing in
    // QTextBlock::blockNumber() on a large document) — fixed now that those are keyed by blockId,
    // resolved through m_blocksById, which blockInfoDestroyed() keeps free of dead ids
    // UNCONDITIONALLY (even mid-m_bulkReset; see its definition). No disconnect/reconnect needed
    // around clear(): rebuildIndex() below still re-derives the correct final state regardless.
    m_bulkReset = true; // every block goes: blocksLoaded below, not one blockRemoved each
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
    m_pendingFont = {};
    rebuildBlockIds(); // first: rebuildIndex() keys its entries by the blockId this assigns
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
    emit blocksLoaded();
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
    m_pendingFont = {};
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
    c.mergeCharFormat(m_pendingFont);
    return c;
}

void RichTextHandler::mergeCharFormat(const QTextCharFormat &fmt)
{
    QTextCursor c = cursor();
    if (c.isNull())
        return;
    // A bare caret in an empty paragraph: this cursor is ours and temporary, so merging into it
    // would be lost — the block's char format is what the next typed character takes (and what
    // the editor's own cursor reads there).
    if (!c.hasSelection() && c.block().length() == 1)
        c.mergeBlockCharFormat(fmt);
    else
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
    if (m_pendingFont.hasProperty(QTextFormat::FontFamilies) && m_selectionStart == m_selectionEnd)
        return m_pendingFont.fontFamilies().toStringList().value(0);
    return effectiveFontProperty(rt::Prop::FontFamily).toString();
}

qreal RichTextHandler::fontSize() const
{
    // effectiveFontProperty() already normalizes to canonical (zoom-1) units itself — a DIRECT
    // hit is live-document units (baked at the zoom in effect) and needs /m_zoom; a value from
    // the resolved style chain (m_sheet is never scaled) is already canonical and must NOT be
    // divided again — only effectiveFontProperty's direct-vs-chain branch knows which is which.
    if (m_pendingFont.hasProperty(QTextFormat::FontPointSize) && m_selectionStart == m_selectionEnd)
        return m_pendingFont.fontPointSize() / m_zoom;
    const QVariant v = effectiveFontProperty(rt::Prop::FontSize);
    return v.isValid() ? v.toDouble() : 0.0; // unset: the editor's own base font stands in
}

QVariant RichTextHandler::effectiveFontProperty(rt::Prop prop) const
{
    const QTextCursor c = m_selectionStart == m_selectionEnd ? insertionCursor() : cursor();
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (c.isNull() || !doc)
        return QVariant();
    const int from = c.selectionStart(), to = c.selectionEnd();
    const QString key = prop == rt::Prop::FontFamily ? QStringLiteral("fontFamily") : QStringLiteral("fontSize");
    // The EFFECTIVE value at one run: direct/baked character formatting wins outright (what
    // restyle() bakes onto the live format for every style EXCEPT Default Paragraph Style's own
    // font — ★2 docs); otherwise resolve the full style chain — paragraph style then any char
    // style on top — WITHOUT restyle()'s skipDocumentFont suppression, so a Default Paragraph
    // Style the app itself customized (newDocument(options)) is reported too. Only when NOTHING
    // in the chain defines it (including an unmodified Default Paragraph Style) does it come back
    // unset, meaning "the editor's own base font stands in" — this handler has no idea what that
    // font actually is (that lives in the QML layer), so it cannot report a value for it.
    //
    // Units: a DIRECT FontSize is baked into the live document AT THE ZOOM IN EFFECT (like
    // setFontSize() does) — bring it back down to canonical before comparing/returning. A value
    // from the style chain is already canonical (m_sheet is never scaled, ★2) — untouched.
    const auto effectiveAt = [&](const QTextCharFormat &cf, const QString &paraStyle) -> QVariant {
        QVariant direct = rt::readProp(cf, prop);
        if (direct.isValid()) {
            if (prop == rt::Prop::FontSize)
                direct = direct.toDouble() / m_zoom;
            return direct;
        }
        QVariantMap resolved = m_sheet.resolved(paraStyle, false);
        if (const QString cs = rt::charStyleOf(cf); !cs.isEmpty()) {
            const QVariantMap fromCharStyle = m_sheet.resolved(cs, false);
            for (auto it = fromCharStyle.cbegin(); it != fromCharStyle.cend(); ++it)
                resolved.insert(it.key(), it.value());
        }
        return resolved.value(key);
    };
    // Across a real selection, every
    // run touched must agree — Word's "blank box" convention for a mixed selection (office item
    // 2b, ★4 addendum): the FIRST disagreement (including "set here, unset there") stops early.
    // Blocks in document order already include table cells and nested frames — no recursion.
    QVariant value;
    bool any = false;
    for (QTextBlock b = doc->findBlock(from); b.isValid() && b.position() < to; b = b.next()) {
        const QString ps = rt::paragraphStyleOf(b);
        for (auto fit = b.begin(); !fit.atEnd(); ++fit) {
            const QTextFragment frag = fit.fragment();
            if (!frag.isValid())
                continue;
            const int s = qMax(frag.position(), from);
            const int e = qMin(frag.position() + frag.length(), to);
            if (s >= e)
                continue;
            const QVariant v = effectiveAt(frag.charFormat(), ps);
            if (!any) {
                value = v;
                any = true;
            } else if (v.isValid() != value.isValid() || (v.isValid() && v != value)) {
                return QVariant(); // mixed
            }
        }
    }
    // No run touched (a bare caret, e.g. in an empty paragraph): the caret's own format — what
    // the next typed character gets — over its paragraph's style chain.
    if (!any)
        return effectiveAt(c.charFormat(), rt::paragraphStyleOf(c.block()));
    return value;
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

void RichTextHandler::mergeInsertionFont(const QTextCharFormat &fmt)
{
    if (m_selectionStart != m_selectionEnd) {
        m_pendingFont = {};
        mergeCharFormat(fmt);
        return;
    }
    if (!m_document)
        return;
    m_pendingFont.merge(fmt);
    // Set Qt's live cursor, so key events, IME commits and insertText share the same
    // insertion format. A cursor-only format neither edits text nor adds an undo step.
    if (auto *edit = qobject_cast<QQuickTextEdit *>(m_document->parent())) {
        auto *d = static_cast<QQuickTextEditPrivate *>(QQuickItemPrivate::get(edit));
        auto *control = static_cast<QQuickTextControlPrivate *>(QObjectPrivate::get(d->control));
        // QQuickTextControl's methods are not exported by release Qt builds. Its cursor
        // is the insertion state used by key/IME events; do not change its position here.
        if (!control->cursor.hasSelection() && control->cursor.position() == m_cursorPosition)
            control->cursor.mergeCharFormat(fmt);
    }
    emit formatChanged();
}

void RichTextHandler::setFontFamily(const QString &name)
{
    if (name.trimmed().isEmpty())
        return;
    QTextCharFormat f;
    f.setProperty(QTextFormat::FontFamilies, QStringList { name });
    mergeInsertionFont(f);
}

void RichTextHandler::setFontSize(qreal pt)
{
    if (pt <= 0)
        return;
    QTextCharFormat f;
    f.setProperty(QTextFormat::FontPointSize, pt * m_zoom); // canonical in, baked in at the current zoom
    mergeInsertionFont(f);
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

void RichTextHandler::saveOdfCopyAsync(const QUrl &file)
{
    if (m_copyRunning) {
        m_queuedCopy = file;
        return;
    }
    if (!m_doc) {
        emit saveCopyFinished(false, QStringLiteral("No document attached"));
        return;
    }
    // QTextDocument::clone() copies through a fragment and then inserts it into a
    // second document. Keep the immutable, reentrant fragment as our snapshot and do
    // that second copy on the worker. IDs are not part of a fragment's user data.
    QTextDocumentFragment snapshot;
    std::unique_ptr<QTextDocument> imageSnapshot;
    const QStringList ids = blockIds();
    const QTextFrameFormat rootFormat = m_doc->rootFrame()->frameFormat();
    const QUrl baseUrl = m_doc->baseUrl();
    QList<QPair<QUrl, QVariant>> images;
    for (const auto &format : m_doc->allFormats()) {
        if (!format.isImageFormat()) continue;
        const QUrl name(format.toImageFormat().name());
        QVariant resource = m_doc->resource(QTextDocument::ImageResource, name);
        if (resource.metaType().id() == QMetaType::QPixmap)
            resource = resource.value<QPixmap>().toImage();
        images.append({name, resource});
    }
    if (images.isEmpty()) {
        snapshot = QTextDocumentFragment(m_doc);
    } else {
        // Qt lazily creates image handlers in a fragment's internal document when
        // inserting it. That QObject still has UI affinity. Use a detached clone for
        // image-bearing documents so every resource handler is created on its owner thread.
        imageSnapshot.reset(m_doc->clone());
        for (const auto &image : images)
            imageSnapshot->addResource(QTextDocument::ImageResource, image.first, image.second);
        imageSnapshot->moveToThread(nullptr);
    }
    QPromise<QString> promise;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        const QString error = watcher->result();
        watcher->deleteLater();
        QPointer<RichTextHandler> guard(this);
        setError(error);
        if (!guard) return;
        emit saveCopyFinished(error.isEmpty(), error);
        if (!guard) return;
        m_copyRunning = false;
        if (auto next = std::exchange(m_queuedCopy, std::nullopt))
            saveOdfCopyAsync(*next);
    });
    watcher->setFuture(promise.future());
    m_copyRunning = true;
    QThreadPool::globalInstance()->start([snapshot = std::move(snapshot), imageSnapshot = std::move(imageSnapshot),
                                        ids, rootFormat, baseUrl, sheet = m_sheet,
                                        zoom = m_zoom, path = localPath(file), promise = std::move(promise)]() mutable {
        promise.start();
        std::unique_ptr<QTextDocument> document = std::move(imageSnapshot);
        if (document) {
            document->moveToThread(QThread::currentThread());
        } else {
            document = std::make_unique<QTextDocument>();
            document->setUndoRedoEnabled(false);
            document->setLayoutEnabled(false);
            document->setBaseUrl(baseUrl);
            QTextCursor(document.get()).insertFragment(snapshot);
            document->rootFrame()->setFrameFormat(rootFormat);
        }
        auto block = document->begin();
        for (const auto &id : ids) {
            block.setUserData(new rt::BlockInfo(id));
            block = block.next();
        }
        QString error;
        rt::writeOdt(path, document.get(), sheet, zoom, &error);
        promise.addResult(error);
        promise.finish();
    });
}

bool RichTextHandler::loadOdf(const QUrl &file)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return false;
    QString why;
    rt::StyleSheet sheet;
    // readOdt() calls doc->clear() internally when it does touch the document — see newDocument()
    // for why that no longer requires disconnecting handleContentsChange around it.
    m_bulkReset = true; // see newDocument(): a load replaces every block at once
    const bool ok = rt::readOdt(localPath(file), doc, &sheet, &why);
    if (!ok && sheet.isEmpty()) { // not found / not an .odt: the document was not touched
        m_bulkReset = false;
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
    m_pendingFont = {};
    rebuildBlockIds(); // the file's xml:id, generated where absent — first: rebuildIndex() needs it
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
    emit blocksLoaded();
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
    if (m_pendingFont.hasProperty(QTextFormat::FontPointSize))
        m_pendingFont.setFontPointSize(m_pendingFont.fontPointSize() * ratio);
    if (doc) {
        QTextCursor edit(doc);
        edit.beginEditBlock(); // one undo step, like any other formatting change
        rescaleRange(-1, -1, ratio, false);
        edit.endEditBlock();
    }
    m_zoom = z;
    if (m_selectionStart == m_selectionEnd && !m_pendingFont.isEmpty())
        mergeInsertionFont(m_pendingFont);
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

// ─── blocks (office item 3) ────────────────────────────────────────────────────────────────────
//
// Identity lives in each block's QTextBlockUserData (rt::BlockInfo). Verified against Qt 6.11
// (qtextdocument_p.cpp): insert_block() keeps the ORIGINAL node for the text before the split
// point and gives the text after it a NEW node (no user data); remove_block() keeps the upper
// block and erases the lower one — except an empty upper block, which is the one erased; an
// erased node's QTextBlockData::free() deletes its user data, so ~BlockInfo is the exact removal
// notice. QTextBlock::isValid() is only `p && n` (a removed block's handle stays "valid"), which
// is why nothing here dereferences a handle that may be gone: m_blocksById drops an id in
// ~BlockInfo, before contentsChange is even emitted.

namespace {
constexpr qsizetype kMaxIdSteps = 1000; // undo-id history kept (Qt's own stack is unbounded)
}

void RichTextHandler::blockInfoDestroyed(rt::BlockInfo *info)
{
    // m_blocksById must drop the id UNCONDITIONALLY, even mid-bulk-reset: it is the liveness index
    // handleContentsChange()'s headings/counts splice (★3/item 7) resolves every surviving entry
    // through, and a doc->clear() (newDocument/loadOdf) can fire an intermediate contentsChange
    // while m_bulkReset is still true — see the note above trackBlocks(). The removedNow/signal
    // bookkeeping below stays skipped during a bulk reset: rebuildBlockIds() wipes and rebuilds it
    // from scratch right after, so queuing one blockRemoved per block would be wasted work.
    m_blocksById.remove(info->id);
    m_pageBreaks.remove(info->id);
    if (m_bulkReset)
        return;
    m_removedNow.append(info->id);
}

void RichTextHandler::detachBlockInfos()
{
    if (!m_doc)
        return;
    for (QTextBlock b = m_doc->begin(); b.isValid(); b = b.next())
        if (rt::BlockInfo *info = rt::blockInfo(b))
            info->owner = nullptr;
}

QString RichTextHandler::newBlockId()
{
    // An XML NCName (written as xml:id), short, unique in this document.
    QString id;
    do
        id = QLatin1Char('p') + QString::number(m_nextBlockId++, 36);
    while (m_blocksById.contains(id));
    return id;
}

void RichTextHandler::rebuildBlockIds()
{
    m_bulkReset = false;
    m_blocksById.clear();
    m_removedNow.clear();
    m_removedQueue.clear();
    m_changedQueue.clear();
    m_idSteps.clear();
    m_newUndoCommand = false;
    if (!m_marks.isEmpty()) { // marks belong to the document they were added to
        m_marks.clear();
        scheduleMarksRepaint();
    }
    QTextDocument *doc = m_doc;
    m_lastUndoState = doc && doc->isUndoRedoEnabled() ? doc->availableUndoSteps() : 0;
    if (!doc)
        return;
    m_blocksById.reserve(doc->blockCount());
    // Pass 1: the ids the blocks already carry (a file's xml:id) win, first occurrence only.
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        rt::BlockInfo *info = rt::blockInfo(b);
        if (!info)
            continue;
        info->owner = this;
        info->changePending = false;
        info->textHash = qHash(b.text());
        if (!info->id.isEmpty() && !m_blocksById.contains(info->id))
            m_blocksById.insert(info->id, b);
        else
            info->id.clear();
    }
    // Pass 2: every other block gets a generated one.
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        rt::BlockInfo *info = rt::blockInfo(b);
        if (!info) {
            info = new rt::BlockInfo(QString());
            info->owner = this;
            info->textHash = qHash(b.text());
            b.setUserData(info);
        }
        if (info->id.isEmpty()) {
            info->id = newBlockId();
            m_blocksById.insert(info->id, b);
        }
    }
}

void RichTextHandler::trackBlocks(int position, int charsRemoved, int charsAdded)
{
    QTextDocument *doc = m_doc;
    if (!doc || m_bulkReset)
        return;
    // 1. What replayed: undoCommandAdded fires (before contentsChange) only for a NEW edit; an
    //    undo/redo moves the stack state without it.
    const bool undoOn = doc->isUndoRedoEnabled();
    const int state = undoOn ? doc->availableUndoSteps() : 0;
    enum class Kind : quint8 { Edit, Undo, Redo };
    Kind kind = Kind::Edit;
    if (undoOn && state < m_lastUndoState)
        kind = Kind::Undo;
    else if (undoOn && state > m_lastUndoState && !m_newUndoCommand)
        kind = Kind::Redo;
    const IdStep *replay = nullptr;
    if (kind != Kind::Edit) {
        const int key = kind == Kind::Undo ? m_lastUndoState : state;
        for (const IdStep &s : std::as_const(m_idSteps))
            if (s.state == key) {
                replay = &s;
                break;
            }
    }

    // 2. The edited span (post-edit): new blocks have no BlockInfo; a kept block whose text hash
    //    moved changed its TEXT (a format-only edit leaves the hash alone).
    const int last = doc->characterCount() - 1;
    QTextBlock b = doc->findBlock(qBound(0, position, last));
    const QTextBlock end = doc->findBlock(qBound(0, position + charsAdded, last));
    QVarLengthArray<QTextBlock, 8> fresh;
    bool textChanged = charsRemoved != charsAdded;
    for (; b.isValid(); b = b.next()) {
        if (rt::BlockInfo *info = rt::blockInfo(b)) {
            const size_t h = qHash(b.text());
            if (info->textHash != h) {
                info->textHash = h;
                queueBlockChange(info);
                textChanged = true;
            }
        } else {
            fresh.append(b);
        }
        if (b == end)
            break;
    }
    QStringList added;
    if (!fresh.isEmpty()) {
        // Undo recreates exactly the blocks the step removed (and redo the ones it created), in
        // document order — give them their ids back when the counts agree.
        const QStringList *reuse = nullptr;
        if (replay) {
            const QStringList &ids = kind == Kind::Undo ? replay->removed : replay->added;
            if (ids.size() == fresh.size())
                reuse = &ids;
        }
        for (qsizetype i = 0; i < fresh.size(); ++i) {
            QString id = reuse ? reuse->at(i) : QString();
            if (id.isEmpty() || m_blocksById.contains(id))
                id = newBlockId();
            auto *info = new rt::BlockInfo(id);
            info->owner = this;
            info->textHash = qHash(fresh[i].text());
            fresh[i].setUserData(info);
            m_blocksById.insert(id, fresh[i]);
            queueBlockChange(info);
            if (kind == Kind::Edit)
                added.append(id);
        }
    }
    QStringList removed = std::exchange(m_removedNow, {});
    textChanged = textChanged || !fresh.isEmpty() || !removed.isEmpty();
    // Marks follow the text (a format-only change leaves them alone); the overlay redraws either
    // way — the layout under them may have moved.
    if (!m_marks.isEmpty()) {
        if (textChanged)
            adjustMarks(position, charsRemoved, charsAdded);
        scheduleMarksRepaint();
    }

    // 3. A new edit: the redo history Qt just dropped goes from ours too; a structural edit
    //    (blocks created/removed) is recorded under the stack state it ends at.
    if (kind == Kind::Edit && undoOn) {
        m_idSteps.removeIf([this](const IdStep &s) { return s.state > m_lastUndoState; });
        if (!added.isEmpty() || !removed.isEmpty()) {
            if (!m_idSteps.isEmpty() && m_idSteps.last().state == state) {
                m_idSteps.last().removed += removed;
                m_idSteps.last().added += added;
            } else {
                m_idSteps.append(IdStep { state, removed, added });
                if (m_idSteps.size() > kMaxIdSteps)
                    m_idSteps.removeFirst();
            }
        }
    }
    m_newUndoCommand = false;
    m_lastUndoState = state;
    if (!removed.isEmpty()) {
        m_removedQueue += removed;
        scheduleBlockSignals();
    }
}

void RichTextHandler::queueBlockChange(rt::BlockInfo *info)
{
    if (info->changePending)
        return;
    info->changePending = true;
    m_changedQueue.append(info->id);
    scheduleBlockSignals();
}

void RichTextHandler::scheduleBlockSignals()
{
    if (m_signalsQueued)
        return;
    m_signalsQueued = true;
    QMetaObject::invokeMethod(this, &RichTextHandler::flushBlockSignals, Qt::QueuedConnection);
}

void RichTextHandler::flushBlockSignals()
{
    m_signalsQueued = false;
    const QStringList removed = std::exchange(m_removedQueue, {});
    const QStringList changed = std::exchange(m_changedQueue, {});
    if (!m_doc)
        return;
    for (const QString &id : removed)
        if (!m_blocksById.contains(id)) // not brought back (undo) in the meantime
            emit blockRemoved(id);
    for (const QString &id : changed) {
        const auto it = m_blocksById.constFind(id);
        if (it == m_blocksById.cend())
            continue; // removed after the change: blockRemoved said it
        rt::BlockInfo *info = rt::blockInfo(*it);
        if (!info || !info->changePending)
            continue;
        info->changePending = false;
        emit blockChanged(id);
    }
}

QStringList RichTextHandler::blockIds() const
{
    QStringList out;
    if (!m_doc)
        return out;
    out.reserve(m_doc->blockCount());
    for (QTextBlock b = m_doc->begin(); b.isValid(); b = b.next())
        if (const rt::BlockInfo *info = rt::blockInfo(b))
            out.append(info->id);
    return out;
}

QString RichTextHandler::blockIdAt(int position) const
{
    if (!m_doc || position < 0 || position >= m_doc->characterCount())
        return QString();
    const rt::BlockInfo *info = rt::blockInfo(m_doc->findBlock(position));
    return info ? info->id : QString();
}

QString RichTextHandler::blockText(const QString &blockId) const
{
    const auto it = m_blocksById.constFind(blockId);
    if (it == m_blocksById.cend())
        return QString();
    return it->text().replace(QChar::LineSeparator, QLatin1Char('\n')); // same length: offsets hold
}

// ─── marks (office item 3) ─────────────────────────────────────────────────────────────────────
//
// Marks are an OVERLAY: absolute document positions in one sorted list, shifted on each text
// edit (O(marks), a tight loop over a contiguous array) and drawn by MarksOverlay, a child item of
// the editor stacked UNDER its text (z < 0). Nothing touches the QTextDocument, so marks are
// never saved, copied, undone, counted as `modified` nor relayout anything.
//
// Why not a QSyntaxHighlighter / QTextLayout additional formats (verified, Qt 6.11
// qquicktextnodeengine.cpp mergeFormats()): Qt Quick's text node engine keeps only additional
// formats carrying a foreground, a background or an anchor, and draws underlines as straight
// rectangles from QFont::underline() — a WaveUnderline / SpellCheckUnderline format is not
// drawn by QQuickTextEdit at all. Setting layout formats also re-lays-out the block.

namespace {

// The squiggle: one tile of a sine wave (period 4px) per colour, repeated by image nodes.
constexpr qreal kWaveTile = 256.0;

class MarksRootNode final : public QSGNode {
public:
    ~MarksRootNode() override { qDeleteAll(m_waves); }
    QSGTexture *wave(QQuickWindow *window, const QColor &color, qreal dpr)
    {
        if (!qFuzzyCompare(dpr, m_dpr)) {
            qDeleteAll(m_waves);
            m_waves.clear();
            m_dpr = dpr;
        }
        if (QSGTexture *t = m_waves.value(color.rgba()))
            return t;
        const qreal h = RichTextHandler::kWaveHeight;
        QImage img(qCeil(kWaveTile * dpr), qCeil(h * dpr), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(dpr, dpr);
        p.setPen(QPen(color, 1.0));
        const qreal mid = h / 2, amp = h / 2 - 0.75;
        QPainterPath path(QPointF(0, mid));
        for (qreal x = 0.25; x <= kWaveTile; x += 0.25)
            path.lineTo(x, mid + amp * qSin(x * M_PI / 2));
        p.drawPath(path);
        p.end();
        QSGTexture *t = window->createTextureFromImage(img);
        m_waves.insert(color.rgba(), t);
        return t;
    }

private:
    QHash<QRgb, QSGTexture *> m_waves;
    qreal m_dpr = 0;
};

} // namespace

// Viewport-bounded: it observes the viewport (QQuickItem::ItemObservesViewport, public since Qt
// 6.9: ItemTransformHasChanged reaches it while the Flickable scrolls) and only lays out the marks
// of a band a quarter viewport above and below the visible part; it is re-polished when the visible
// part leaves the band, or when the marks / the text / the size change.
class MarksOverlay final : public QQuickItem {
public:
    MarksOverlay(RichTextHandler *handler, QQuickItem *editor) : QQuickItem(editor), m_handler(handler)
    {
        setFlag(ItemHasContents);
        setFlag(ItemObservesViewport);
        setZ(-1); // under the editor's own text (and its selection)
        setAcceptedMouseButtons(Qt::NoButton);
        setSize(editor->size());
        connect(editor, &QQuickItem::widthChanged, this, [this, editor] { setWidth(editor->width()); });
        connect(editor, &QQuickItem::heightChanged, this, [this, editor] { setHeight(editor->height()); });
    }
    void schedule() { polish(); }

protected:
    bool active() const { return m_handler && (m_handler->hasMarks() || !m_segments.isEmpty()); }
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override
    {
        QQuickItem::geometryChange(newGeometry, oldGeometry);
        if (active())
            polish();
    }
    void itemChange(ItemChange change, const ItemChangeData &value) override
    {
        if (change == ItemTransformHasChanged && active() && !m_band.contains(clipRect()))
            polish();
        QQuickItem::itemChange(change, value);
    }
    void updatePolish() override
    {
        // A small margin around the visible part: laying a mark out shapes its line (QTextLine::
        // cursorToX), and a shaped line keeps its glyphs — exactly the blocks QQuickTextEdit
        // itself renders (its own viewport culling), not pages around them.
        const QRectF clip = clipRect();
        const qreal margin = clip.height() / 4;
        m_band = clip.adjusted(0, -margin, 0, margin);
        QList<RichTextHandler::MarkSegment> segs = m_handler && !clip.isEmpty() ? m_handler->markSegments(m_band)
                                                                                 : QList<RichTextHandler::MarkSegment>();
        if (segs == m_segments)
            return; // scrolled within the same marks: the nodes stand
        m_segments = std::move(segs);
        update();
    }
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override
    {
        auto *root = static_cast<MarksRootNode *>(old);
        if (m_segments.isEmpty()) {
            delete root;
            return nullptr;
        }
        if (!root)
            root = new MarksRootNode;
        while (QSGNode *c = root->firstChild()) {
            root->removeChildNode(c);
            delete c;
        }
        QQuickWindow *w = window();
        const qreal dpr = w->effectiveDevicePixelRatio();
        for (const RichTextHandler::MarkSegment &s : std::as_const(m_segments)) {
            if (s.shape != RichTextHandler::MarkSegment::Wave) {
                QSGRectangleNode *n = w->createRectangleNode();
                n->setRect(s.rect);
                n->setColor(s.color);
                root->appendChildNode(n);
                continue;
            }
            QSGTexture *tex = root->wave(w, s.color, dpr);
            for (qreal x = 0; x < s.rect.width(); x += kWaveTile) {
                const qreal wdt = qMin(kWaveTile, s.rect.width() - x);
                QSGImageNode *n = w->createImageNode();
                n->setTexture(tex);
                n->setOwnsTexture(false);
                n->setFiltering(QSGTexture::Linear);
                n->setRect(QRectF(s.rect.x() + x, s.rect.y(), wdt, s.rect.height()));
                n->setSourceRect(QRectF(0, 0, wdt * dpr, s.rect.height() * dpr));
                root->appendChildNode(n);
            }
        }
        return root;
    }

private:
    QPointer<RichTextHandler> m_handler;
    QRectF m_band;
    QList<RichTextHandler::MarkSegment> m_segments;
};

namespace {
QQuickItem *createMarksOverlay(RichTextHandler *handler, QQuickItem *editor) { return new MarksOverlay(handler, editor); }
} // namespace

void RichTextHandler::scheduleMarksRepaint()
{
    if (m_overlay)
        static_cast<MarksOverlay *>(m_overlay.data())->schedule();
}

int RichTextHandler::kindIndex(const QString &kind)
{
    if (const qsizetype i = m_kinds.indexOf(kind); i >= 0)
        return int(i);
    // Defaults: spelling a red wave, grammar a blue wave, highlight a yellow background; any
    // other kind a grey single underline until the app styles it (markStyle).
    MarkStyle st { 1, QColor(0x80, 0x80, 0x80), QColor() };
    if (kind == QLatin1String("spelling"))
        st = { 2, QColor(0xd3, 0x2f, 0x2f), QColor() };
    else if (kind == QLatin1String("grammar"))
        st = { 2, QColor(0x1e, 0x66, 0xf5), QColor() };
    else if (kind == QLatin1String("highlight"))
        st = { 0, QColor(), QColor(0xff, 0xf1, 0x76) };
    m_kinds.append(kind);
    m_kindStyles.append(st);
    return int(m_kinds.size() - 1);
}

void RichTextHandler::ensureMarksSorted()
{
    if (m_marksSorted)
        return;
    std::stable_sort(m_marks.begin(), m_marks.end(), [](const Mark &a, const Mark &b) { return a.start < b.start; });
    m_marksSorted = true;
}

QString RichTextHandler::addMark(const QString &kind, const QString &blockId, int start, int end, const QString &id)
{
    const auto it = m_blocksById.constFind(blockId);
    if (it == m_blocksById.cend() || start < 0 || start >= end || end > it->length() - 1)
        return QString();
    const int pos = it->position();
    Mark m { pos + start, pos + end, kindIndex(kind), id.isEmpty() ? QLatin1Char('m') + QString::number(m_nextMarkId++, 36) : id };
    if (!m_marks.isEmpty() && m.start < m_marks.last().start)
        m_marksSorted = false; // the app's first pass goes in document order: append stays sorted
    m_marks.append(std::move(m));
    scheduleMarksRepaint();
    return m_marks.last().id;
}

void RichTextHandler::clearMarks(const QString &kind, const QString &blockId)
{
    const qsizetype k = m_kinds.indexOf(kind);
    if (k < 0 || m_marks.isEmpty())
        return;
    int from = 0, to = std::numeric_limits<int>::max();
    if (!blockId.isEmpty()) {
        const auto it = m_blocksById.constFind(blockId);
        if (it == m_blocksById.cend())
            return;
        from = it->position();
        to = from + it->length();
    }
    const qsizetype removed = m_marks.removeIf([&](const Mark &m) { return m.kind == k && m.start >= from && m.start < to; });
    if (removed)
        scheduleMarksRepaint();
}

QString RichTextHandler::markStyle(const QString &kind, const QVariantMap &style)
{
    MarkStyle st = m_kindStyles[kindIndex(kind)];
    for (auto it = style.cbegin(); it != style.cend(); ++it) {
        const QVariant &v = it.value();
        const bool unset = !v.isValid() || v.isNull();
        if (it.key() == QLatin1String("underline")) {
            const QString u = unset ? QStringLiteral("none") : v.toString();
            if (u == QLatin1String("none")) st.underline = 0;
            else if (u == QLatin1String("single")) st.underline = 1;
            else if (u == QLatin1String("wave")) st.underline = 2;
            else return QStringLiteral("underline: expected \"wave\", \"single\" or \"none\"");
        } else if (it.key() == QLatin1String("color") || it.key() == QLatin1String("background")) {
            QColor c;
            if (!unset) {
                c = QColor::fromString(v.toString());
                if (!c.isValid())
                    return QStringLiteral("%1: not a color: \"%2\"").arg(it.key(), v.toString());
            }
            (it.key() == QLatin1String("color") ? st.color : st.background) = c;
        } else {
            return QStringLiteral("unknown mark style property \"%1\"").arg(it.key());
        }
    }
    m_kindStyles[m_kinds.indexOf(kind)] = st;
    scheduleMarksRepaint();
    return QString();
}

QVariant RichTextHandler::markAt(int position)
{
    if (!m_doc || m_marks.isEmpty() || position < 0 || position >= m_doc->characterCount())
        return QVariant();
    ensureMarksSorted();
    const QTextBlock b = m_doc->findBlock(position);
    const int bpos = b.position();
    // The last mark starting at or before `position`, walking back within the block.
    auto it = std::upper_bound(m_marks.cbegin(), m_marks.cend(), position, [](int p, const Mark &m) { return p < m.start; });
    while (it != m_marks.cbegin()) {
        --it;
        if (it->start < bpos)
            break;
        if (it->end > position) {
            const rt::BlockInfo *info = rt::blockInfo(b);
            return QVariantMap {
                { QStringLiteral("kind"), m_kinds.value(it->kind) },
                { QStringLiteral("id"), it->id },
                { QStringLiteral("blockId"), info ? info->id : QString() },
                { QStringLiteral("start"), it->start - bpos },
                { QStringLiteral("end"), it->end - bpos },
            };
        }
    }
    return QVariant();
}

void RichTextHandler::adjustMarks(int position, int charsRemoved, int charsAdded)
{
    // Old-document range [position, position + charsRemoved) became charsAdded new characters.
    // Before it: unchanged. After it: shifted. Touching it — overlapping what was removed, or an
    // insertion strictly inside — dropped (a spell result for edited text is stale). An insertion
    // exactly at a mark's start shifts it; at its end leaves it as it was.
    ensureMarksSorted();
    const int editEnd = position + charsRemoved;
    const int delta = charsAdded - charsRemoved;
    qsizetype w = 0;
    const qsizetype n = m_marks.size();
    Mark *m = m_marks.data();
    for (qsizetype i = 0; i < n; ++i) {
        Mark &k = m[i];
        if (k.end <= position) {
            // before the edit
        } else if (k.start >= editEnd) {
            k.start += delta;
            k.end += delta;
        } else {
            continue; // dropped
        }
        if (w != i)
            m[w] = std::move(k);
        ++w;
    }
    if (w != n)
        m_marks.resize(w);
}

QList<RichTextHandler::MarkSegment> RichTextHandler::markSegments(const QRectF &band)
{
    QList<MarkSegment> out;
    QTextDocument *doc = m_doc;
    if (!doc || m_marks.isEmpty())
        return out;
    ensureMarksSorted();
    QAbstractTextDocumentLayout *layout = doc->documentLayout();

    // Document → editor item coordinates: the editor's own caret rectangle at 0 against the
    // layout's (QQuickTextEdit adds its padding and alignment offsets — xoff/yoff).
    QPointF offset;
    const QTextBlock first = doc->begin();
    QPointF docOrigin = layout->blockBoundingRect(first).topLeft();
    if (const QTextLine l0 = first.layout()->lineForTextPosition(0); l0.isValid())
        docOrigin += QPointF(l0.cursorToX(0), l0.y());
    if (QObject *editor = m_document ? m_document->parent() : nullptr) {
        QRectF r;
        if (QMetaObject::invokeMethod(editor, "positionToRectangle", Qt::DirectConnection, Q_RETURN_ARG(QRectF, r), Q_ARG(int, 0)))
            offset = r.topLeft() - docOrigin;
    }

    // The marks whose block may meet the band: binary search (marks are in document order, and
    // so are block tops in the root frame) for the first one whose block reaches the band's
    // top — O(log marks) block lookups, never a walk over the document. A table's cells are in
    // row order, not strictly by y: the walk stops a full band past the bottom, not at it.
    const auto blockBox = [&](int pos) { return layout->blockBoundingRect(doc->findBlock(pos)).translated(offset); };
    auto it = m_marks.cbegin();
    if (!band.isNull())
        it = std::partition_point(m_marks.cbegin(), m_marks.cend(), [&](const Mark &m) { return blockBox(m.start).bottom() < band.top(); });
    const qreal stopAt = band.isNull() ? std::numeric_limits<qreal>::max() : band.bottom() + band.height();
    QTextBlock block;
    int bpos = 0, bend = -1;
    QRectF box;
    for (; it != m_marks.cend(); ++it) {
        const MarkStyle &st = m_kindStyles[it->kind];
        if (!st.background.isValid() && (st.underline == 0 || !st.color.isValid()))
            continue;
        if (it->start >= bend) {
            block = doc->findBlock(it->start);
            bpos = block.position();
            bend = bpos + block.length();
            box = layout->blockBoundingRect(block).translated(offset);
            if (box.top() > stopAt)
                break;
        }
        if (!band.isNull() && (box.top() > band.bottom() || box.bottom() < band.top()))
            continue;
        const QPointF origin = box.topLeft();
        const QTextLayout *tl = block.layout();
        const int s = it->start - bpos, e = it->end - bpos;
        for (int i = 0, lines = tl->lineCount(); i < lines; ++i) {
            const QTextLine line = tl->lineAt(i);
            const int ls = line.textStart(), le = ls + line.textLength();
            if (le <= s)
                continue;
            if (ls >= e)
                break;
            qreal x1 = line.cursorToX(qMax(s, ls)), x2 = line.cursorToX(qMin(e, le));
            if (x1 > x2)
                std::swap(x1, x2); // right-to-left
            const qreal top = origin.y() + line.y();
            if (!band.isNull() && (top > band.bottom() || top + line.height() < band.top()))
                continue;
            const QRectF lineBox(origin.x() + x1, top, x2 - x1, line.height());
            if (st.background.isValid())
                out.append({ lineBox, st.background, MarkSegment::Background });
            const qreal baseline = top + line.ascent();
            if (st.underline == 1 && st.color.isValid())
                out.append({ QRectF(lineBox.x(), baseline + 1, lineBox.width(), qMax(1.0, qreal(qRound(line.height() / 16.0)))),
                             st.color, MarkSegment::Underline });
            else if (st.underline == 2 && st.color.isValid())
                out.append({ QRectF(lineBox.x(), qMin(baseline + 1, top + line.height() - kWaveHeight), lineBox.width(), kWaveHeight),
                             st.color, MarkSegment::Wave });
        }
    }
    return out;
}

bool RichTextHandler::replaceRange(const QString &blockId, int start, int end, const QString &text)
{
    const auto it = m_blocksById.constFind(blockId);
    if (!m_doc || it == m_blocksById.cend() || start < 0 || start > end || end > it->length() - 1)
        return false;
    const int pos = it->position();
    QTextCursor c(m_doc);
    // The format of the first replaced character (charFormat() reads the one BEFORE the cursor);
    // a pure insertion takes the caret's, like typing.
    c.setPosition(pos + (end > start ? start + 1 : start));
    QTextCharFormat fmt = c.charFormat();
    if (fmt.isImageFormat())
        fmt = it->charFormat();
    c.beginEditBlock(); // one undo step
    c.setPosition(pos + start);
    c.setPosition(pos + end, QTextCursor::KeepAnchor);
    c.removeSelectedText(); // within the block: it keeps its node, hence its id
    const QStringList parts = text.split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < parts.size(); ++i) {
        if (i > 0)
            breakParagraph(c);
        if (!parts[i].isEmpty())
            c.insertText(parts[i], fmt);
    }
    c.endEditBlock();
    emit formatChanged();
    return true;
}

// Page breaks are view decorations: only the nearby marker items exist, and changing
// their label or geometry never changes the QTextDocument or its undo history.
class PageBreakMarker final : public QQuickPaintedItem {
public:
    explicit PageBreakMarker(QQuickItem *parent) : QQuickPaintedItem(parent) {
        setObjectName(QStringLiteral("pageBreakMarker"));
        setAcceptedMouseButtons(Qt::NoButton);
        setAntialiasing(true);
    }
    void setLabel(const QString &text, qreal zoom) {
        if (m_text == text && m_zoom == zoom) return;
        m_text = text;
        m_zoom = zoom;
        setProperty("label", text);
        update();
    }
    void paint(QPainter *p) override {
        QFont font;
        font.setPixelSize(qMax(1, qRound(10 * m_zoom)));
        p->setFont(font);
        const qreal gap = QFontMetricsF(font).horizontalAdvance(m_text) + 12 * m_zoom;
        const qreal center = width() / 2;
        QPen pen(QColor(QStringLiteral("#808080")), 1, Qt::DashLine);
        p->setPen(pen);
        p->drawLine(QPointF(0, height() / 2), QPointF(qMax(0.0, center - gap / 2), height() / 2));
        p->drawLine(QPointF(qMin(width(), center + gap / 2), height() / 2), QPointF(width(), height() / 2));
        p->drawText(boundingRect(), Qt::AlignCenter, m_text);
    }
private:
    QString m_text;
    qreal m_zoom = 1;
};

class PageBreakOverlay final : public QQuickItem {
public:
    PageBreakOverlay(RichTextHandler *handler, QQuickItem *editor) : QQuickItem(editor), m_handler(handler) {
        setFlag(ItemObservesViewport);
        setAcceptedMouseButtons(Qt::NoButton);
        setSize(editor->size());
        connect(editor, &QQuickItem::widthChanged, this, [this, editor] { setWidth(editor->width()); polish(); });
        connect(editor, &QQuickItem::heightChanged, this, [this, editor] { setHeight(editor->height()); polish(); });
        connect(handler, &RichTextHandler::contentsChanged, this, [this] { polish(); });
        connect(handler, &RichTextHandler::pageBreakLabelChanged, this, [this] { polish(); });
        connect(handler, &RichTextHandler::zoomChanged, this, [this] { polish(); });
        auto *doc = handler->document()->textDocument();
        connect(doc->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] { polish(); });
        if (auto *edit = qobject_cast<QQuickTextEdit *>(editor)) {
            connect(edit, &QQuickTextEdit::topPaddingChanged, this, [this] { polish(); });
            connect(edit, &QQuickTextEdit::leftPaddingChanged, this, [this] { polish(); });
            connect(edit, &QQuickTextEdit::rightPaddingChanged, this, [this] { polish(); });
        }
        polish();
    }
protected:
    void itemChange(ItemChange change, const ItemChangeData &value) override {
        if (change == ItemTransformHasChanged && m_handler &&
            (m_handler->hasPageBreaks() || !m_items.isEmpty())) polish();
        QQuickItem::itemChange(change, value);
    }
    void updatePolish() override {
        if (!m_handler) return;
        const QRectF band = clipRect().adjusted(0, -32, 0, 32);
        const auto segments = m_handler->pageBreakSegments(band);
        QSet<QString> visible;
        for (const auto &segment : segments) {
            visible.insert(segment.id);
            auto *item = m_items.value(segment.id);
            if (!item) {
                item = new PageBreakMarker(this);
                item->setProperty("blockId", segment.id);
                m_items.insert(segment.id, item);
            }
            item->setPosition(segment.rect.topLeft());
            item->setSize(segment.rect.size());
            item->setLabel(m_handler->pageBreakLabel(), m_handler->zoom());
        }
        for (auto it = m_items.begin(); it != m_items.end();) {
            if (!visible.contains(it.key())) {
                delete it.value();
                it = m_items.erase(it);
            } else ++it;
        }
    }
private:
    QPointer<RichTextHandler> m_handler;
    QHash<QString, PageBreakMarker *> m_items;
};

namespace {
QQuickItem *createPageBreakOverlay(RichTextHandler *handler, QQuickItem *editor) {
    return new PageBreakOverlay(handler, editor);
}
}

QList<RichTextHandler::PageBreakSegment> RichTextHandler::pageBreakSegments(const QRectF &band) const
{
    QList<PageBreakSegment> result;
    if (!m_doc || m_pageBreaks.isEmpty()) return result;
    auto *edit = qobject_cast<QQuickTextEdit *>(m_document->parent());
    if (!edit) return result;
    auto *layout = m_doc->documentLayout();
    const QTextBlock first = m_doc->begin();
    QPointF origin = layout->blockBoundingRect(first).topLeft();
    if (const auto line = first.layout()->lineForTextPosition(0); line.isValid())
        origin += QPointF(line.cursorToX(0), line.y());
    const QPointF offset = edit->positionToRectangle(0).topLeft() - origin;
    const qreal height = 14 * m_zoom;
    const qreal left = edit->leftPadding() + m_doc->documentMargin();
    const qreal width = qMax(0.0, edit->width() - left - edit->rightPadding() - m_doc->documentMargin());
    for (const auto &id : m_pageBreaks) {
        const auto it = m_blocksById.constFind(id);
        if (it == m_blocksById.cend()) continue;
        const qreal top = layout->blockBoundingRect(*it).top() + offset.y();
        const QRectF rect(left, top - height, width, height);
        if (band.isNull() || band.intersects(rect)) result.append({id, rect});
    }
    return result;
}

void RichTextHandler::setPageBreakLabel(const QString &label)
{
    if (m_pageBreakLabel == label) return;
    m_pageBreakLabel = label;
    emit pageBreakLabelChanged();
}
