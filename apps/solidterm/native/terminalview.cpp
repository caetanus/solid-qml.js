#include "terminalview.h"

#include "autoscroll.h"
#include "linkscan.h"
#include "perftrace.h"
#include "stallwatch.h"
#include "mousereport.h"
#include "scrollanchor.h"
#include "tmux/client.h"
#include "tmux/keys.h"

#include "qmlcss/componentcache.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QFileInfo>
#include <QInputMethodEvent>
#include <QKeySequence>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTimer>
#include <QQuickWindow>
#include <QSocketNotifier>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTexture>
#include <QSGVertexColorMaterial>
#include <QUrl>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

// ─── GPU glyph material ───────────────────────────────────────────────────────────────────────────
// Samples the coverage atlas (.r) and tints by the per-vertex foreground colour (premultiplied in
// the fragment shader). One instance per TerminalView; its texture is the item's glyph atlas.
namespace {

struct GlyphVertex { float x, y, u, v, r, g, b, a, isColor; };

const QSGGeometry::Attribute kGlyphAttrs[] = {
    QSGGeometry::Attribute::create(0, 2, QSGGeometry::FloatType, true),  // pos
    QSGGeometry::Attribute::create(1, 2, QSGGeometry::FloatType, false), // uv
    QSGGeometry::Attribute::create(2, 4, QSGGeometry::FloatType, false), // color
    QSGGeometry::Attribute::create(3, 1, QSGGeometry::FloatType, false), // isColor (emoji flag)
};
const QSGGeometry::AttributeSet kGlyphAttrSet = { 4, sizeof(GlyphVertex), kGlyphAttrs };

class GlyphMaterial : public QSGMaterial {
public:
    GlyphMaterial() { setFlag(Blending, true); }
    QSGMaterialType *type() const override { static QSGMaterialType t; return &t; }
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial *o) const override {
        const auto *m = static_cast<const GlyphMaterial *>(o);
        if (texture == m->texture)
            return 0;
        const qint64 a = texture ? texture->comparisonKey() : 0;
        const qint64 b = m->texture ? m->texture->comparisonKey() : 0;
        return a < b ? -1 : 1;
    }
    // The material OWNS its atlas texture, so it's freed on the render thread when the node tree is
    // torn down — never touched from the GUI thread. `texture` is the raw view the shader binds.
    void setTexture(QSGTexture *t) { m_tex.reset(t); texture = t; }
    QSGTexture *texture = nullptr;

private:
    std::unique_ptr<QSGTexture> m_tex;
};

class GlyphShader : public QSGMaterialShader {
public:
    GlyphShader()
    {
        setShaderFileName(VertexStage, QStringLiteral(":/solidterm/shaders/glyph.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/solidterm/shaders/glyph.frag.qsb"));
    }
    bool updateUniformData(RenderState &state, QSGMaterial *, QSGMaterial *) override
    {
        QByteArray *buf = state.uniformData();
        bool changed = false;
        if (state.isMatrixDirty()) {
            const QMatrix4x4 m = state.combinedMatrix();
            memcpy(buf->data(), m.constData(), 64);
            changed = true;
        }
        if (state.isOpacityDirty()) {
            const float o = state.opacity();
            memcpy(buf->data() + 64, &o, 4);
            changed = true;
        }
        return changed;
    }
    void updateSampledImage(RenderState &state, int binding, QSGTexture **texture,
                            QSGMaterial *newMaterial, QSGMaterial *) override
    {
        if (binding != 1)
            return;
        auto *mat = static_cast<GlyphMaterial *>(newMaterial);
        if (!mat->texture)
            return;
        mat->texture->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
        *texture = mat->texture;
    }
};

QSGMaterialShader *GlyphMaterial::createShader(QSGRendererInterface::RenderMode) const
{
    return new GlyphShader;
}

} // namespace

// tmux mode (docs/solidterm-tmux-control-mode.md 3.1): which pane of which control client this view
// shows. The client owns the feed pipe; the view only hands its read end to the parser thread.
struct TerminalView::TmuxLink {
    std::shared_ptr<tmux::Client> client;
    int pane = -1;
    QString cwd;
    std::string literal;      // text for one send-keys -l (coalesced keystrokes / IME commits)
    std::string bytes;        // raw bytes for send-keys -H (mouse, focus, paste, alt-screen scroll)
    bool flushQueued = false;
    QString colors;           // the OSC 10/11 colours last reported
    bool colorsQueued = false;  // one report per event-loop turn (style setters come in bursts)
    void (*beforeInput)(void *, int) = nullptr;
    void *inputCtx = nullptr;
    void willSend() { if (beforeInput) beforeInput(inputCtx, pane); }
};

void TerminalView::setTmuxInputHook(void (*hook)(void *ctx, int pane), void *ctx)
{
    if (!m_tmux)
        return;
    m_tmux->beforeInput = hook;
    m_tmux->inputCtx = ctx;
}

// ─── tmux mode input (docs/solidterm-tmux-control-mode.md 3.3) ────────────────────────────────────

void TerminalView::tmuxKeyEvent(QKeyEvent *event)
{
    const auto ucs4 = event->text().toUcs4();
    const tmux::KeyInput in = tmux::keyInput(event->key(), event->modifiers(), reinterpret_cast<const char32_t *>(ucs4.constData()), int(ucs4.size()));
    if (in.kind == tmux::KeyInput::Name)
        tmuxSendKeyName(in.text);
    else if (in.kind == tmux::KeyInput::Literal)
        tmuxQueueLiteral(in.text);
}

void TerminalView::tmuxSendKeyName(const std::string &name)
{
    tmuxFlush(); // what was typed before goes first
    m_tmux->willSend();
    tmux::CommandLine l;
    l.cmd("send-keys").pane("-t", m_tmux->pane).arg(name);
    m_tmux->client->send(l, tmux::Client::KIgnore);
}

void TerminalView::tmuxQueueLiteral(const std::string &utf8)
{
    if (utf8.empty())
        return;
    if (!m_tmux->bytes.empty())
        tmuxFlush();
    m_tmux->literal += utf8;
    if (!m_tmux->flushQueued) {
        m_tmux->flushQueued = true;
        QMetaObject::invokeMethod(this, [this] { if (m_tmux) tmuxFlush(); }, Qt::QueuedConnection);
    }
}

void TerminalView::tmuxQueueBytes(const char *data, size_t len)
{
    if (!len)
        return;
    if (!m_tmux->literal.empty())
        tmuxFlush();
    m_tmux->bytes.append(data, len);
    if (!m_tmux->flushQueued) {
        m_tmux->flushQueued = true;
        QMetaObject::invokeMethod(this, [this] { if (m_tmux) tmuxFlush(); }, Qt::QueuedConnection);
    }
}

void TerminalView::tmuxFlush()
{
    TmuxLink &t = *m_tmux;
    t.flushQueued = false;
    if (!t.literal.empty() || !t.bytes.empty())
        t.willSend();
    constexpr size_t kChunk = 1024; // payload bytes per command (a -H byte costs 3 on the line)
    // Text: send-keys -l (literal; "--" so text starting with '-' is not read as a flag). Cut on
    // UTF-8 boundaries.
    size_t at = 0;
    while (at < t.literal.size()) {
        size_t n = std::min(kChunk, t.literal.size() - at);
        while (at + n < t.literal.size() && n > 1 && (static_cast<unsigned char>(t.literal[at + n]) & 0xc0) == 0x80)
            --n;
        tmux::CommandLine l;
        l.cmd("send-keys").pane("-t", t.pane).flag("-l").flag("--").arg(std::string_view(t.literal).substr(at, n));
        t.client->send(l, tmux::Client::KIgnore);
        at += n;
    }
    t.literal.clear();
    // Bytes: send-keys -H, written raw to the pane (cmd-send-keys.c → input-keys.c).
    static const char hex[] = "0123456789abcdef";
    for (at = 0; at < t.bytes.size(); at += kChunk) {
        const size_t n = std::min(kChunk, t.bytes.size() - at);
        tmux::CommandLine l;
        l.cmd("send-keys").pane("-t", t.pane).flag("-H");
        for (size_t k = 0; k < n; ++k) {
            const unsigned char b = static_cast<unsigned char>(t.bytes[at + k]);
            const char h[3] = { hex[b >> 4], hex[b & 15], 0 };
            l.flag(h);
        }
        t.client->send(l, tmux::Client::KIgnore);
    }
    t.bytes.clear();
}

void TerminalView::tmuxScheduleColors()
{
    if (!m_tmux || m_tmux->colorsQueued)
        return;
    m_tmux->colorsQueued = true;
    QMetaObject::invokeMethod(this, [this] {
        if (m_tmux) {
            m_tmux->colorsQueued = false;
            tmuxReportColors();
        }
    }, Qt::QueuedConnection);
}

void TerminalView::tmuxReportColors()
{
    if (!m_tmux)
        return;
    auto rgb = [](const QColor &c) {
        return QStringLiteral("rgb:%1/%2/%3")
            .arg(c.red() * 257, 4, 16, QLatin1Char('0'))
            .arg(c.green() * 257, 4, 16, QLatin1Char('0'))
            .arg(c.blue() * 257, 4, 16, QLatin1Char('0'));
    };
    const QString fg = rgb(m_foreground), bg = rgb(m_background);
    if (m_tmux->colors == fg + bg)
        return;
    m_tmux->colors = fg + bg;
    // refresh-client -r '%P:<report>' (tmux 3.5+): what tmux answers a program's OSC 10/11 query
    // with, so programs that adapt to a light or dark background see SolidTerm's colours.
    const std::string pane = "%" + std::to_string(m_tmux->pane) + ":";
    for (const auto &[osc, value] : { std::pair<const char *, QString>("10", fg), std::pair<const char *, QString>("11", bg) }) {
        tmux::CommandLine l;
        l.cmd("refresh-client").flag("-r").arg(pane + "\x1b]" + osc + ";" + value.toStdString() + "\x1b\\");
        m_tmux->client->send(l, tmux::Client::KIgnore);
    }
}

void TerminalView::pasteBracketed(const QString &text)
{
    if (!m_vt || m_readOnly)
        return;
    // tmux mode: libvterm wraps the paste in ESC[200~ … ESC[201~ when the program turned bracketed
    // paste on (its mode comes from the same raw stream tmux parsed); the bytes go as send-keys -H.
    const auto ucs4 = text.toUcs4();
    ScreenLock lock(m_screenMutex);
    vterm_keyboard_start_paste(m_vt);
    for (const char32_t c : ucs4)
        vterm_keyboard_unichar(m_vt, c, VTERM_MOD_NONE);
    vterm_keyboard_end_paste(m_vt);
}

void TerminalView::setTmuxPane(std::shared_ptr<tmux::Client> client, int pane)
{
    if (m_started || !client)
        return; // only before the session starts
    m_tmux = std::make_unique<TmuxLink>();
    m_tmux->client = std::move(client);
    m_tmux->pane = pane;
}

int TerminalView::tmuxPane() const
{
    return m_tmux ? m_tmux->pane : -1;
}

void TerminalView::setTmuxCwd(const QString &cwd)
{
    if (m_tmux)
        m_tmux->cwd = cwd;
}

void TerminalView::setImposedGrid(int rows, int cols)
{
    if (rows == m_imposedRows && cols == m_imposedCols)
        return;
    m_imposedRows = rows;
    m_imposedCols = cols;
    if (isComponentComplete())
        applyGrid();
}

QString TerminalView::dumpScreen(const TermScreen &term)
{
    QString out;
    if (!term.isAttached())
        return out;
    const int total = term.sbCount() + term.rows();
    for (int r = 0; r < total; ++r) {
        const TermScreen::Line l = term.line(r);
        QString line;
        for (int c = 0; l.cells && c < l.cols; ++c) {
            const TermCell &cell = l.cells[c];
            if (cell.chars[0] == kWideContinuation)
                continue;
            if (!cell.chars[0]) {
                line += QLatin1Char(' ');
                continue;
            }
            int n = 0;
            while (n < VTERM_MAX_CHARS_PER_CELL && cell.chars[n])
                ++n;
            line += QString::fromUcs4(reinterpret_cast<const char32_t *>(cell.chars), n);
        }
        while (line.endsWith(QLatin1Char(' ')))
            line.chop(1);
        out += line;
        out += QLatin1Char('\n');
    }
    VTermPos pos;
    vterm_state_get_cursorpos(term.state(), &pos);
    out += QStringLiteral("cursor %1,%2\nalt %3\nsb %4\n").arg(pos.row).arg(pos.col).arg(term.isAltScreen() ? 1 : 0).arg(term.sbCount());
    return out;
}

QList<QList<qint64>> TerminalView::debugGridLog() const
{
    ScreenLock lock(m_screenMutex);
    return m_gridLog;
}

QString TerminalView::debugDump() const
{
    ScreenLock lock(m_screenMutex);
    return dumpScreen(m_term);
}

TerminalView::TerminalView(QQuickItem *parent)
    : QQuickItem(parent)
{
    m_font = QFont(QStringLiteral("monospace"));
    m_font.setPixelSize(15);
    m_font.setStyleHint(QFont::Monospace);
    // Right is normally captured by the solid <ContextMenu>'s MouseArea, which sits ABOVE this item
    // and only declines (mouse.accepted = false) when the running program wants mouse reports and
    // Shift isn't held — see wantsMouseReport(). Declared here too so THAT right-press (forwarded by
    // Qt Quick's own hit-testing once the MouseArea declines) actually reaches mousePressEvent.
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton | Qt::RightButton);
    setActiveFocusOnTab(true);
    setAcceptHoverEvents(true);               // hover to detect/underline links
    setFlag(ItemHasContents, true);
    // Without this Qt's compose/IME layer never engages for the terminal: a dead key is dropped
    // and the next letter arrives bare (pt-BR ´ + a typed "a", not "á").
    setFlag(ItemAcceptsInputMethod, true);

    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(530);
    connect(m_blinkTimer, &QTimer::timeout, this, [this] {
        if (!hasActiveFocus() || !m_cursorVisible || m_scrollOffset > 0)
            return;
        m_blinkOn = !m_blinkOn;
        update();
    });
    m_blinkTimer->start();
    connect(this, &QQuickItem::activeFocusChanged, this, [this] {
        resetBlink();
        updateFocusReport();
        // Losing focus mid-drag (a popup/dialog/menu grabbing it, window deactivation) is one of the
        // ways a local selection drag can end without ever reaching mouseReleaseEvent.
        if (!hasActiveFocus() && m_selecting) {
            m_selecting = false;
            stopAutoscroll();
            resumeOutputFromSelection();
        }
    });

    // Visual bell: BEL raises a brief bright wash, cleared ~130ms later. bellRang is emitted from the
    // vterm callback on the GUI thread, so this runs synchronously here (update() is safe).
    connect(this, &TerminalView::bellRang, this, [this] {
        m_bellActive = true;
        update();
        QTimer::singleShot(130, this, [this] { m_bellActive = false; update(); });
        // Urgency hint: when the window isn't focused, ask the WM for attention (taskbar flash / X11
        // urgency / wlr activation) so a bell in a background terminal is noticed. alert(0) stays until
        // the window is activated again.
        if (QWindow *w = window(); w && !w->isActive())
            w->alert(0);
    });

    // Single-threaded mode only (the parser thread reads the pty itself otherwise).
    connect(&m_pty, &PtySession::bytesRead, this, &TerminalView::parseOnGuiThread);
    connect(&m_pty, &PtySession::finished, this, &TerminalView::sessionFinished);
}

TerminalView::~TerminalView()
{
    // ~QQuickItem runs after this body and after every member is gone, and it still emits signals
    // of this item — activeFocusChanged when a focused pane closes — whose handlers here
    // (resetBlink, updateFocusReport → vterm_state_focus_out) would touch the freed VTerm and the
    // destroyed members. Cut this item's connections to itself first. (A use-after-free found by the
    // ThreadSanitizer stress run closing focused panes; it predates the parser thread.)
    QObject::disconnect(this, nullptr, this, nullptr);
    // The parser thread first: after stop() nothing but this thread touches m_vt/m_term. It never
    // waits on the GUI thread, so the join cannot deadlock (a slice in progress finishes first).
    delete m_newsNotifier;
    m_newsNotifier = nullptr;
    m_worker.stop();
    if (m_tmux) // the worker no longer reads the feed pipe: the client may close it
        m_tmux->client->releasePane(m_tmux->pane);
    if (m_vt)
        vterm_free(m_vt);
    m_vt = nullptr;
}

bool TerminalView::parserThreadEnabled()
{
    static const bool on = qEnvironmentVariable("SOLIDTERM_PARSER_THREAD") != QLatin1String("0");
    return on;
}

// ─── parsing: the parser thread's sink, or the GUI thread in single-threaded mode ─────────────────

// One batch per slice/read: the TermScreen hooks only record news, which is applied (scrollChanged,
// blink restart, update) once per GUI wake — a `seq` scrolls ~500 lines per 4 KiB, and per-line
// scrollChanged() re-ran the QML scrollbar's bindings for every line (26% of the main thread).
bool TerminalView::wkFeed(void *user, const char *data, size_t len)
{
    auto *self = static_cast<TerminalView *>(user);
    if (Q_UNLIKELY(PerfTrace::on) && PerfTrace::floodKeys && std::memchr(data, PerfTrace::echoChar, len))
        PerfTrace::stage(PerfTrace::PtyRead); // this slice carries the injected key's echo
    self->m_term.beginBatch();
    self->m_fedBytes += len;
    self->m_inFeed = true; // libvterm's output now = replies to queries in the stream
    vterm_input_write(self->m_vt, data, len);
    self->m_inFeed = false;
    if (self->m_term.endBatch()) {
        self->m_news.damaged = true;
        if (Q_UNLIKELY(PerfTrace::on))
            PerfTrace::stage(PerfTrace::Damage);
    }
    return self->m_news.any();
}

void TerminalView::wkExited(void *user)
{
    static_cast<TerminalView *>(user)->m_news.exited = true;
}

void TerminalView::wkReadProbe(void *user, size_t len)
{
    if (Q_UNLIKELY(PerfTrace::on)) {
        if (!PerfTrace::floodKeys)
            PerfTrace::stage(PerfTrace::PtyRead);
        PerfTrace::ptyReads.fetch_add(1, std::memory_order_relaxed);
        PerfTrace::ptyBytes.fetch_add(int64_t(len), std::memory_order_relaxed);
    }
    if (Q_UNLIKELY(StallWatch::on))
        static_cast<TerminalView *>(user)->m_pty.noteRead(len);
}

static const TermWorkerSink kWorkerSink = {
    /*feed*/ TerminalView::wkFeed,
    /*exited*/ TerminalView::wkExited,
    /*readProbe*/ TerminalView::wkReadProbe,
};

void TerminalView::parseOnGuiThread(const QByteArray &bytes)
{
    if (!m_vt)
        return;
    if (Q_UNLIKELY(PerfTrace::on)) {
        if (!PerfTrace::floodKeys)
            PerfTrace::stage(PerfTrace::PtyRead);
        PerfTrace::ptyReads.fetch_add(1, std::memory_order_relaxed);
        PerfTrace::ptyBytes.fetch_add(bytes.size(), std::memory_order_relaxed);
    }
    {
        ScreenLock lock(m_screenMutex); // uncontended here: no parser thread in this mode
        wkFeed(this, bytes.constData(), size_t(bytes.size()));
    }
    applyNews();
}

// libvterm's output callback (keys, paste, mouse/focus reports, DA/DSR responses). Always called
// with the lock held: from a slice on the parser thread or from a GUI input handler.
void TerminalView::writeToPty(const char *s, size_t len)
{
    // tmux mode: replies libvterm produces WHILE PARSING (DA, DSR, DECRQM…) never go — tmux answered
    // those queries itself (3.2). What it produces from the GUI (mouse and focus reports, paste,
    // alternate-screen wheel arrows) goes to the pane as raw bytes (send-keys -H). Keys never get
    // here in tmux mode: they go as key names / literals (tmuxKeyEvent).
    if (m_tmux) {
        if (!m_inFeed)
            tmuxQueueBytes(s, len);
        return;
    }
    if (Q_UNLIKELY(PerfTrace::on))
        PerfTrace::stage(PerfTrace::PtyWrite);
    if (m_threaded)
        m_worker.queueOutput(s, len); // non-blocking; the worker writes what the kernel defers
    else
        m_pty.writeBytes(QByteArray(s, int(len)));
}

int TerminalView::effectiveOffset(int sbCount, uint64_t pushed) const
{
    // Output never moves a scrolled-back view (VTE/Tilix default): each line pushed since the GUI
    // last looked moves the view one further from live, capped at the oldest line — the same result
    // as offsetAfterPush() once per line.
    if (m_scrollOffset <= 0)
        return 0;
    const uint64_t moved = pushed - m_seenPushed;
    return int(std::min<uint64_t>(uint64_t(m_scrollOffset) + moved, uint64_t(std::max(sbCount, 0))));
}

// GUI thread, after lines left the scrollback for good (lineanchor.h): the selection is clipped to
// the oldest retained line or cleared when wholly gone (VTE), evicted search matches are dropped
// (the current one keeps pointing at the same match), and an evicted hover link is forgotten.
void TerminalView::dropEvictedAnchors(uint64_t base)
{
    m_lineBase = base;
    if (m_selValid) {
        CellPos from, to;
        selectedRange(from, to);
        switch (clipEvicted(from, to, base)) {
        case Eviction::Kept:
            break;
        case Eviction::Clipped: // only the earlier end is gone: it becomes the oldest line's start
            (m_selAnchor.line < oldestLine(base) ? m_selAnchor : m_selEnd) = from;
            break;
        case Eviction::Gone:
            if (!m_selecting) {
                m_selValid = false;
                emit selectionChanged();
            }
            break;
        }
    }
    int gone = 0; // m_matches is in line order (search walks oldest → newest)
    while (gone < m_matches.size() && m_matches[gone].line < oldestLine(base))
        ++gone;
    if (gone > 0) {
        m_matches.remove(0, gone);
        m_searchIndex = m_matches.isEmpty() ? -1 : qMax(0, m_searchIndex - gone);
        emit searchChanged(m_searchIndex + 1, int(m_matches.size()));
    }
    if (m_hoverLink.valid && m_hoverLink.line < oldestLine(base)) {
        m_hoverLink = {};
        emit hoverLinkChanged(QString());
    }
}

// Lock held: the line on the view's top row as the next frame will show it (the parser may be ahead
// of the GUI's copies) — the renderer's own anchoring rule, so a click lands on what is drawn.
LineNo TerminalView::topLineLocked() const
{
    const int sbSize = m_term.sbCount();
    return viewTopLine(m_term.lineBase(), sbSize, clampOffset(effectiveOffset(sbSize, m_term.pushedTotal()), sbSize));
}

// GUI thread: take what libvterm reported (on whichever thread) and act on it.
void TerminalView::applyNews()
{
    News n;
    int sb;
    uint64_t pushed, base;
    {
        ScreenLock lock(m_screenMutex);
        n = m_news;
        m_news.title.clear();
        m_news.titleChanged = m_news.cursorMoved = m_news.scrollMoved = m_news.damaged = m_news.exited = false;
        m_news.bells = 0;
        sb = m_term.sbCount();
        pushed = m_term.pushedTotal();
        base = m_term.lineBase();
    }
    bool scroll = n.scrollMoved || pushed != m_seenPushed || sb != m_sbCount;
    if (base != m_lineBase)
        dropEvictedAnchors(base); // text that left the scrollback takes its highlights with it
    if (scroll) {
        m_scrollOffset = clampOffset(effectiveOffset(sb, pushed), sb);
        m_seenPushed = pushed;
        m_sbCount = sb;
    }
    if (n.altScreen != m_altScreen) {
        m_altScreen = n.altScreen;
        scroll = true; // the scrollbar hides on the alt screen (vim/less), see altScreen()
        // The live rows now show the other screen: a selection there would highlight unrelated text
        // (the copy-on-select PRIMARY selection still holds what was selected).
        if (m_selValid && !m_selecting) {
            m_selValid = false;
            emit selectionChanged();
        }
    }
    m_mouseMode = n.mouseMode; // decideMouseAction() (mousereport.h) reads it on every mouse event
    const bool cursorChanged = n.cursor.row != m_cursor.row || n.cursor.col != m_cursor.col
        || n.cursorVisible != m_cursorVisible;
    m_cursor = n.cursor;
    m_cursorVisible = n.cursorVisible;
    if (n.titleChanged) {
        m_title = n.title;
        emit titleChanged();
    }
    if (scroll)
        emit scrollChanged();
    if (n.cursorMoved)
        resetBlink(); // solid cursor on activity (calls update())
    if (n.damaged || cursorChanged || scroll)
        update();
    if (n.bells)
        emit bellRang(); // one flash for a burst of BELs
    if (n.exited && !m_tmux)
        m_pty.markExited(); // → finished() → sessionFinished(); a tmux pane's lifetime is tmux's
}

// ─── libvterm plumbing ──────────────────────────────────────────────────────────────────────────

// Content changed outside a pty batch (resize, reset): repaint. Inside a batch TermScreen makes no
// call at all — the bytesRead handler update()s once at its end.
// Content changed outside a parse batch (resize, reset): recorded; the caller applies the news.
void TerminalView::cbDamage(void *user)
{
    if (Q_UNLIKELY(PerfTrace::on))
        PerfTrace::stage(PerfTrace::Damage);
    static_cast<TerminalView *>(user)->m_news.damaged = true;
}

int TerminalView::cbMoveCursor(VTermPos pos, VTermPos, int visible, void *user)
{
    News &n = static_cast<TerminalView *>(user)->m_news;
    n.cursor = pos;
    n.cursorVisible = visible != 0;
    n.cursorMoved = true;
    return 1;
}

int TerminalView::cbSetTermProp(VTermProp prop, VTermValue *val, void *user)
{
    auto *self = static_cast<TerminalView *>(user);
    News &n = self->m_news;
    if (prop == VTERM_PROP_TITLE) {
        // libvterm hands a string over in FRAGMENTS when the OSC is split across input writes
        // (a read boundary, a tmux %output line): collect from the initial one, commit on the final
        // one. Taking each fragment as the whole title kept only its tail ("passthrough" for
        // "from-passthrough").
        if (val->string.initial)
            self->m_titleAcc.clear();
        self->m_titleAcc.append(val->string.str, qsizetype(val->string.len));
        if (!val->string.final)
            return 1;
        n.title = QString::fromUtf8(self->m_titleAcc);
        self->m_titleAcc.clear();
        n.titleChanged = true;
    } else if (prop == VTERM_PROP_CURSORVISIBLE) {
        n.cursorVisible = val->boolean;
        n.damaged = true;
    } else if (prop == VTERM_PROP_MOUSE) {
        n.mouseMode = val->number; // DECSET 1000/1002/1003 (CLICK/DRAG/MOVE) or NONE
    } else if (prop == VTERM_PROP_ALTSCREEN) {
        n.altScreen = val->boolean;
        n.scrollMoved = true;
    }
    return 1;
}

int TerminalView::cbBell(void *user)
{
    ++static_cast<TerminalView *>(user)->m_news.bells;
    return 1;
}

void TerminalView::itemChange(ItemChange change, const ItemChangeData &data)
{
    // Cross-window move (pane detach/reattach): our cached scene-graph nodes and the glyph-atlas
    // texture belong to the OLD window's render context. Drop the cached pointers (Qt frees the old
    // nodes on the scene change) and force an atlas re-upload so updatePaintNode rebuilds fresh in the
    // new window — otherwise it would touch stale nodes and hang/crash on the new render thread.
    if (change == ItemSceneChange) {
        m_bgNode = m_glyphNode = m_cursorNode = nullptr;
        m_imageNode = nullptr;
        m_glyphs.reset();
        m_snapValid = false; // copy every row on the next frame
        // Window close (data.window == nullptr, item leaving the scene for good) or pane detach/
        // reattach (data.window == the new window) both end any local selection drag that was live —
        // either way, pty reads must resume rather than stay frozen forever.
        if (m_selecting) {
            m_selecting = false;
            stopAutoscroll();
        }
        resumeOutputFromSelection();
        update();
    }
    // Focus reporting (DECSET 1004) cares about the WINDOW's activation, not just this item's — a
    // cross-window pane move (detach/reattach) means the old window's activeChanged must stop
    // driving it. Qt auto-disconnects when either end is destroyed, so this is also safe on teardown.
    if (change == ItemSceneChange) {
        QObject::disconnect(m_winActiveConn);
        m_winActiveConn = data.window
            ? connect(data.window, &QWindow::activeChanged, this, [this] { updateFocusReport(); })
            : QMetaObject::Connection();
    }
    QQuickItem::itemChange(change, data);
    // A new window (or the same window moved to a screen with another scale) changes the device
    // pixel ratio the atlas must be rasterised at: rebuild the font + grid for it.
    if ((change == ItemSceneChange && data.window) || change == ItemDevicePixelRatioHasChanged) {
        applyGrid();
        update();
    }
}

// A line entered the scrollback (TermScreen MOVED the row there; a full ring recycled its oldest).
// The scroll anchoring itself is applied by applyNews()/the renderer from pushedTotal().
void TerminalView::cbSbPushed(int, void *user)
{
    static_cast<TerminalView *>(user)->m_news.scrollMoved = true;
}

// A line left the scrollback (popped back onto the screen when the terminal grew taller, or trimmed
// by a lower limit). TermScreen pads a line pushed while narrower with REAL blanks — a zeroed cell
// has width 0 and hangs libvterm's backfill loop forever (see scrollbackcells.h).
void TerminalView::cbSbPopped(int, void *user)
{
    static_cast<TerminalView *>(user)->m_news.scrollMoved = true; // applyNews() clamps the offset
}

static const TermScreenHooks kTermHooks = {
    /*movecursor*/ TerminalView::cbMoveCursor,
    /*settermprop*/ TerminalView::cbSetTermProp,
    /*bell*/ TerminalView::cbBell,
    /*damage*/ TerminalView::cbDamage,
    /*sbPushed*/ TerminalView::cbSbPushed,
    /*sbPopped*/ TerminalView::cbSbPopped,
};

void TerminalView::ensureSession()
{
    if (m_started)
        return;
    m_started = true;

    m_threaded = parserThreadEnabled();
    m_vt = TermScreen::newVTerm(m_rows, m_cols);
    vterm_set_utf8(m_vt, 1);
    vterm_output_set_callback(m_vt, [](const char *s, size_t len, void *user) {
        static_cast<TerminalView *>(user)->writeToPty(s, len);
    }, this);

    // Our own screen over libvterm's state layer (primary + alt screen + scrollback ring).
    m_term.setScrollbackLimit(m_scrollbackLimit);
    m_term.attach(m_vt, &kTermHooks, this);

    // Default colours feed SGR-reset cells. ANSI 0–15 come from the scheme palette at draw time
    // (toQColor); without one they stay vterm's xterm-256 standard.
    VTermState *state = vterm_obtain_state(m_vt);
    VTermColor fg, bg;
    vterm_color_rgb(&fg, uint8_t(m_foreground.red()), uint8_t(m_foreground.green()), uint8_t(m_foreground.blue()));
    vterm_color_rgb(&bg, uint8_t(m_background.red()), uint8_t(m_background.green()), uint8_t(m_background.blue()));
    vterm_state_set_default_colors(state, &fg, &bg);
    m_term.reset(true);
    // The 256-colour palette, once: libvterm never changes it afterwards (it handles no OSC 4), so
    // the renderer needs no access to VTermState. Same values as vterm_state_convert_color_to_rgb.
    for (int i = 0; i < 256; ++i) {
        VTermColor c;
        vterm_state_get_palette_color(state, i, &c);
        m_indexed[i] = QColor(c.rgb.red, c.rgb.green, c.rgb.blue);
    }
    m_news = News {};

    int fd = -1;
    if (m_tmux) {
        // No pty: the parser thread reads the pane's feed pipe (there is no GUI-thread reader for it).
        m_threaded = true;
        fd = m_tmux->client->openPane(m_tmux->pane);
    } else {
        QString cwd;
        QStringList env;
        if (m_hasStart) {
            cwd = m_start.cwd;
            env = m_start.env;
        } else if (QQuickWindow *w = window()) {
            cwd = w->property("solidtermCwd").toString();
            env = w->property("solidtermEnv").toStringList();
        }
        if (cwd.isEmpty() && env.isEmpty() && s_pendingSpawn) {
            cwd = s_pendingSpawn->cwd;
            env = s_pendingSpawn->env;
        }
        m_pty.start(m_rows, m_cols, QString(), cwd, env, /*readHere*/ !m_threaded);
        fd = m_pty.masterFd();
    }
    // Started paused, reading from the next GUI loop turn on — when the single-threaded notifier
    // would first fire too. The session starts before the item is laid out (the grid is still 2x2);
    // a parser running at once wrapped the shell's first output at 2 columns for good.
    if (m_threaded && fd >= 0
        && m_worker.start(fd, &m_screenMutex, &kWorkerSink, this, /*startPaused*/ true)) {
        m_newsNotifier = new QSocketNotifier(m_worker.guiFd(), QSocketNotifier::Read, this);
        connect(m_newsNotifier, &QSocketNotifier::activated, this, [this] {
            m_worker.guiAcknowledge(); // before the apply: news recorded after it posts a fresh wake
            applyNews();
        });
        QMetaObject::invokeMethod(this, [this] {
            if (m_outputPaused)
                return; // a selection drag already holds it; its resume starts reading
            ScreenLock lock(m_screenMutex);
            m_worker.setPaused(false);
        }, Qt::QueuedConnection);
    }
    applyNews();
    if (m_tmux) { // the grid the view starts with: lifts a resize fence waiting for it
        m_gridLog.append({ 0, m_rows, m_cols });
        m_tmux->client->ackPaneSize(m_tmux->pane, m_cols, m_rows);
        tmuxScheduleColors();
    }
    updateFocusReport(); // sync the program's focus state with whatever we're starting with
}

void TerminalView::componentComplete()
{
    QQuickItem::componentComplete();
    ensureStarted();
}

void TerminalView::ensureStarted()
{
    applyGrid();
    ensureSession();
    composeScrollBar(); // no-op once composed; needs this item's QQmlEngine, available by now
}

void TerminalView::applyGrid()
{
    // The grid uses the GlyphCache's (integer) cell metrics, so every cell lands exactly on an
    // atlas tile — glyphs stay crisp (no fractional placement). setFont is a no-op when unchanged.
    // Rasterised at the window's device pixel ratio (crisp on HiDPI); cell metrics stay logical.
    m_glyphs.setFont(m_font.family(), m_font.pixelSize(), window() ? window()->effectiveDevicePixelRatio() : 1.0);
    m_cellW = m_glyphs.cellWidth();
    m_cellH = m_glyphs.cellHeight();
    const int cols = m_imposedCols > 0 ? m_imposedCols : qMax(2, int(width() / m_cellW));
    const int rows = m_imposedRows > 0 ? m_imposedRows : qMax(2, int(height() / m_cellH));
    if (rows == m_rows && cols == m_cols)
        return;
    m_rows = rows;
    m_cols = cols;
    if (m_vt) {
        {
            // Synchronous hand-off: the parser thread is held off while TermScreen resizes.
            ScreenLock lock(m_screenMutex);
            vterm_set_size(m_vt, rows, cols); // TermScreen resizes (and damages) inside
            if (m_tmux)
                m_gridLog.append({ qint64(m_fedBytes), rows, cols });
        }
        if (m_tmux) // tmux sized the pane already; output for the new size may flow now (3.4)
            m_tmux->client->ackPaneSize(m_tmux->pane, cols, rows);
        else
            m_pty.resize(rows, cols);
        applyNews();
    }
    emit scrollChanged(); // viewportRows moved — the scrollbar's thumb size depends on it
    update();
}

void TerminalView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (isComponentComplete())
        applyGrid();
}

// ─── rendering ──────────────────────────────────────────────────────────────────────────────────

QColor TerminalView::toQColor(VTermColor c, bool isFg) const
{
    if (VTERM_COLOR_IS_DEFAULT_FG(&c))
        return m_foreground;
    if (VTERM_COLOR_IS_DEFAULT_BG(&c))
        return m_background;
    if (VTERM_COLOR_IS_INDEXED(&c))
        return m_hasPalette && c.indexed.idx < 16 ? m_palette[c.indexed.idx] : m_indexed[c.indexed.idx];
    return QColor(c.rgb.red, c.rgb.green, c.rgb.blue);
    Q_UNUSED(isFg);
}

// ─── scene-graph build ────────────────────────────────────────────────────────────────────────────
// Runs on the render thread with the GUI thread blocked (the Quick sync point), so reading the
// vterm screen here is safe. Builds three geometry nodes drawn back-to-front: cell backgrounds +
// selection (vertex colour), glyphs (atlas material), cursor (vertex colour, on top).

namespace {

inline void pushBgQuad(std::vector<QSGGeometry::ColoredPoint2D> &v, qreal x, qreal y, qreal w,
                       qreal h, const QColor &c)
{
    // QSGVertexColorMaterial blends PREMULTIPLIED, so premultiply here — a straight (255,255,255,179)
    // adds to the backdrop instead of blending, saturating translucent white to opaque white.
    const int ca = c.alpha();
    const uchar r = uchar(c.red() * ca / 255), g = uchar(c.green() * ca / 255),
                b = uchar(c.blue() * ca / 255), a = uchar(ca);
    const float x0 = float(x), y0 = float(y), x1 = float(x + w), y1 = float(y + h);
    QSGGeometry::ColoredPoint2D tl, tr, bl, br;
    tl.set(x0, y0, r, g, b, a);
    tr.set(x1, y0, r, g, b, a);
    bl.set(x0, y1, r, g, b, a);
    br.set(x1, y1, r, g, b, a);
    v.insert(v.end(), { tl, tr, bl, bl, tr, br }); // two triangles
}

inline void pushGlyphQuad(std::vector<GlyphVertex> &v, qreal x, qreal y, qreal w, qreal h,
                          const QRectF &uv, const QColor &c, bool colorGlyph)
{
    // The glyph fragment shader is premultiplied (vColor × coverage), so premultiply here — lets a
    // translucent foreground carry alpha < 1 correctly (opaque fg keeps alpha 1, unchanged).
    const float al = float(c.alphaF());
    const float r = float(c.redF()) * al, g = float(c.greenF()) * al, b = float(c.blueF()) * al, a = al;
    const float ic = colorGlyph ? 1.0f : 0.0f;
    const float x0 = float(x), y0 = float(y), x1 = float(x + w), y1 = float(y + h);
    const float u0 = float(uv.left()), v0 = float(uv.top());
    const float u1 = float(uv.right()), v1 = float(uv.bottom());
    const GlyphVertex tl{ x0, y0, u0, v0, r, g, b, a, ic };
    const GlyphVertex tr{ x1, y0, u1, v0, r, g, b, a, ic };
    const GlyphVertex bl{ x0, y1, u0, v1, r, g, b, a, ic };
    const GlyphVertex br{ x1, y1, u1, v1, r, g, b, a, ic };
    v.insert(v.end(), { tl, tr, bl, bl, tr, br });
}

} // namespace

// Render thread, GUI blocked: copy the visible rows (and the scalars the frame needs) out of the
// screen under the lock. In the live view only rows the parser dirtied since the last frame are
// copied; scrolled back, resized, after a DECSCNM flip or a window change, all of them (~212 KB at
// 105x56, tens of microseconds).
void TerminalView::copySnapshot(int &sbCount, int &offset, uint64_t &lineBase, VTermPos &cursor, bool &cursorVisible, bool &reverse)
{
    const int rows = m_rows, cols = m_cols;
    const size_t cells = size_t(rows) * size_t(cols);
    if (m_snapRows != rows || m_snapCols != cols) {
        m_snap.assign(cells, TermCell {});
        m_snapLen.assign(size_t(rows), -1);
        m_snapLive.assign(size_t(rows), 0);
        m_snapRows = rows;
        m_snapCols = cols;
        m_snapValid = false;
    }
    const int64_t tw = Q_UNLIKELY(PerfTrace::on) ? PerfTrace::nowNs() : 0;
    ScreenLock lock(m_screenMutex);
    const int64_t tl = Q_UNLIKELY(PerfTrace::on) ? PerfTrace::nowNs() : 0;
    sbCount = m_term.sbCount();
    offset = clampOffset(effectiveOffset(sbCount, m_term.pushedTotal()), sbCount);
    lineBase = m_term.lineBase();
    reverse = m_term.globalReverse();
    cursor = m_news.cursor;
    cursorVisible = m_news.cursorVisible;
    const bool all = !m_snapValid || offset != 0 || m_snapOffset != 0 || reverse != m_snapReverse
        || m_term.rows() != rows || m_term.cols() != cols;
    for (int row = 0; row < rows; ++row) {
        if (!all && (row >= m_term.rows() || !m_term.rowDirty(row)))
            continue;
        const int liveRow = row - offset;
        TermScreen::Line line;
        if (liveRow < 0) {
            if (sbCount + liveRow >= 0)
                line = m_term.line(sbCount + liveRow);
        } else if (liveRow < m_term.rows()) {
            line = m_term.liveLine(liveRow);
        }
        const size_t at = size_t(row) * size_t(cols);
        if (!line.cells) {
            m_snapLen[size_t(row)] = -1;
            continue;
        }
        const int n = std::min(line.cols, cols);
        std::memcpy(m_snap.data() + at, line.cells, size_t(n) * sizeof(TermCell));
        m_snapLen[size_t(row)] = n;
        m_snapLive[size_t(row)] = line.live ? 1 : 0;
    }
    m_term.clearDirty();
    if (Q_UNLIKELY(PerfTrace::on)) {
        const int64_t te = PerfTrace::nowNs();
        PerfTrace::lockWaitNsTotal.fetch_add(tl - tw, std::memory_order_relaxed);
        PerfTrace::maxInto(PerfTrace::lockWaitNsMax, tl - tw);
        PerfTrace::copyNsTotal.fetch_add(te - tl, std::memory_order_relaxed);
        PerfTrace::maxInto(PerfTrace::copyNsMax, te - tl);
    }
    m_snapOffset = offset;
    m_snapReverse = reverse;
    m_snapValid = true;
}

QSGNode *TerminalView::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data)
{
    if (Q_UNLIKELY(PerfTrace::on)) {
        PerfTrace::stage(PerfTrace::PaintBegin);
        const int64_t t0 = PerfTrace::nowNs();
        QSGNode *n = buildPaintNode(oldNode, data);
        PerfTrace::paint(PerfTrace::nowNs() - t0);
        PerfTrace::stage(PerfTrace::PaintEnd);
        return n;
    }
    return buildPaintNode(oldNode, data);
}

QSGNode *TerminalView::buildPaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    if (!m_term.isAttached() || width() <= 0 || height() <= 0) {
        delete oldNode;
        m_imageNode = nullptr;
        m_bgNode = m_glyphNode = m_cursorNode = nullptr;
        return nullptr;
    }

    // Scratch vertex arrays, reused frame to frame (per render thread: panes in other windows build
    // concurrently) — a fresh vector per frame re-grew through ~11 reallocations of up to 1.2 MB.
    static thread_local std::vector<QSGGeometry::ColoredPoint2D> bg;
    static thread_local std::vector<GlyphVertex> glyphs;
    static thread_local std::vector<QSGGeometry::ColoredPoint2D> cursor;
    bg.clear();
    glyphs.clear();
    cursor.clear();

    // Base background fills the whole item. Its alpha = backgroundOpacity, so at <1 the layer behind
    // (a bg image, or — with transparent chrome — the desktop) shows through the terminal's colour.
    QColor baseBg = m_background;
    baseBg.setAlphaF(m_bgOpacity);
    pushBgQuad(bg, 0, 0, width(), height(), baseBg);

    // The visible rows, copied out of the screen under the lock (the parser thread keeps writing it);
    // everything below reads the copy. Every row's geometry is rebuilt (per-row reuse is proposal P4).
    int sbCount = 0, sbShown = 0;
    uint64_t lineBase = 0;
    VTermPos cursorPos;
    bool cursorVisible = true, globalReverse = false;
    copySnapshot(sbCount, sbShown, lineBase, cursorPos, cursorVisible, globalReverse);
    // Selection, search matches and the hover link are line numbers (lineanchor.h): placed against
    // the SAME base/sbCount/offset the rows were copied with, so they move with their text.
    const LineNo topLine = viewTopLine(lineBase, sbCount, sbShown);
    // Past the end of a scrollback row narrower than the grid: a value-initialised cell, exactly what
    // the previous `VTermScreenCell cell {}` gave (zero colours = RGB black, not the default).
    static const TermCell kPastEnd {};
    for (int row = 0; row < m_rows; ++row) {
        const int lineCols = m_snapLen[size_t(row)];
        if (lineCols < 0)
            continue; // above the oldest scrollback line
        const TermCell *lineCells = m_snap.data() + size_t(row) * size_t(m_snapCols);
        // DECSCNM applies to live rows at read time (scrollback rows have it baked in).
        const bool flipReverse = m_snapLive[size_t(row)] && globalReverse;
        const qreal y = row * m_cellH;

        int col = 0;
        while (col < m_cols) {
            const bool inLine = col < lineCols;
            const TermCell &cell = inLine ? lineCells[col] : kPastEnd;
            const int cw = inLine ? TermScreen::widthAt(lineCells, lineCols, col) : 1;
            const qreal x = col * m_cellW;
            const qreal w = cw * m_cellW;

            QColor fg = toQColor(cell.pen.fg, true);
            QColor bgc = toQColor(cell.pen.bg, false);
            if (cell.pen.reverse ^ flipReverse)
                std::swap(fg, bgc);
            if (bgc != m_background)
                pushBgQuad(bg, x, y, w, m_cellH, bgc);

            // One textured quad per non-blank cell — the atlas makes per-cell quads cheap, so the
            // old run-merge (a QPainter optimisation) is gone.
            // A single codepoint (nearly every cell) is looked up without building a QString; only
            // a real cluster (base + combining marks) pays for one.
            const char32_t c0 = cell.chars[0];
            if (c0 && !(c0 == U' ' && !cell.chars[1])) {
                GlyphCache::Entry e;
                if (!cell.chars[1]) {
                    e = m_glyphs.glyph(c0, cell.pen.bold, cell.pen.italic, cw);
                } else {
                    QString cluster;
                    for (int ci = 0; ci < VTERM_MAX_CHARS_PER_CELL && cell.chars[ci]; ++ci)
                        cluster.append(QString::fromUcs4(&cell.chars[ci], 1));
                    e = m_glyphs.glyph(cluster, cell.pen.bold, cell.pen.italic, cw);
                }
                if (e.valid)
                    pushGlyphQuad(glyphs, x, y, w, m_cellH, e.uv, fg, e.color);
            }
            if (cell.pen.underline)
                pushBgQuad(bg, x, y + m_cellH - 1, w, 1, fg);

            col += cw;
        }
    }

    // Selection: a translucent highlight behind the text (drawn last in the bg node → over cell
    // backgrounds, under glyphs).
    if (m_selValid) {
        CellPos from, to;
        selectedRange(from, to);
        QColor sel = m_foreground;
        sel.setAlpha(64);
        for (int row = 0; row < m_rows; ++row) {
            const LineNo line = topLine + row;
            if (line < from.line || line > to.line || line < oldestLine(lineBase))
                continue;
            const int c0 = line == from.line ? from.col : 0;
            const int c1 = line == to.line ? to.col : m_cols - 1;
            pushBgQuad(bg, c0 * m_cellW, row * m_cellH, (c1 - c0 + 1) * m_cellW, m_cellH, sel);
        }
    }

    // Cursor: a semi-transparent block (focused) so the glyph shows through, or an outline
    // (unfocused). Hidden while scrolled back.
    if (cursorVisible && sbShown == 0) {
        const qreal cx = cursorPos.col * m_cellW, cy = cursorPos.row * m_cellH;
        if (hasActiveFocus()) {
            if (m_blinkOn) {
                QColor cur = m_foreground;
                cur.setAlpha(140);
                pushBgQuad(cursor, cx, cy, m_cellW, m_cellH, cur);
            }
        } else {
            pushBgQuad(cursor, cx, cy, m_cellW, 1, m_foreground);
            pushBgQuad(cursor, cx, cy + m_cellH - 1, m_cellW, 1, m_foreground);
            pushBgQuad(cursor, cx, cy, 1, m_cellH, m_foreground);
            pushBgQuad(cursor, cx + m_cellW - 1, cy, 1, m_cellH, m_foreground);
        }
    }

    // Unfocused split pane: a subtle dark overlay on top of everything (tilix "dim inactive").
    if (m_dimmed)
        pushBgQuad(cursor, 0, 0, width(), height(), QColor(0, 0, 0, 66));

    // Visual bell: a brief bright wash over the pane (audible bells are jarring; tilix defaults to a
    // visual flash). m_bellActive is raised on BEL and cleared by a short timer (see the ctor).
    if (m_bellActive) {
        QColor flash = m_foreground;
        flash.setAlphaF(0.30);
        pushBgQuad(cursor, 0, 0, width(), height(), flash);
    }

    // Search highlights: every match on a visible row (current match brighter).
    if (!m_matches.isEmpty()) {
        for (int mi = 0; mi < m_matches.size(); ++mi) {
            const Match &m = m_matches[mi];
            const int64_t row = viewRowOfLine(topLine, m.line);
            if (row < 0 || row >= m_rows)
                continue;
            const QColor hl = mi == m_searchIndex ? QColor(255, 160, 0, 210) : QColor(230, 200, 0, 90);
            pushBgQuad(bg, m.c0 * m_cellW, row * m_cellH, m.len * m_cellW, m_cellH, hl);
        }
    }

    // Hovered hyperlink: underline it across its span (drawn in the bg layer, at the cell baseline).
    if (m_hoverLink.valid) {
        const int64_t row = viewRowOfLine(topLine, m_hoverLink.line);
        if (row >= 0 && row < m_rows) {
            pushBgQuad(bg, m_hoverLink.c0 * m_cellW, row * m_cellH + m_cellH - 1,
                       (m_hoverLink.c1 - m_hoverLink.c0 + 1) * m_cellW, 1, m_foreground);
        }
    }

    // If the atlas overflowed mid-build (astronomically unlikely), reset it and ask the GUI thread
    // (queued — we're on the render thread here) to re-render with a fresh atlas.
    if (m_glyphs.overflowed()) {
        m_glyphs.reset();
        QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
    }

    QSGNode *root = oldNode;
    if (!root) {
        root = new QSGNode;
        m_imageNode = nullptr;
        m_bgNode = m_glyphNode = m_cursorNode = nullptr;
    }

    // Layer 0 (optional): the background image, cover-fit, below everything.
    if (m_bgImage.isNull()) {
        if (m_imageNode) {
            root->removeChildNode(m_imageNode);
            delete m_imageNode;
            m_imageNode = nullptr;
        }
    } else {
        if (!m_imageNode) {
            m_imageNode = window()->createImageNode();
            m_imageNode->setOwnsTexture(true);
            root->prependChildNode(m_imageNode); // below bg/glyph/cursor
        }
        if (m_bgImageDirty || !m_imageNode->texture()) {
            m_imageNode->setTexture(window()->createTextureFromImage(m_bgImage));
            m_bgImageDirty = false;
        }
        m_imageNode->setRect(0, 0, width(), height());
        const qreal iw = qMax(1, m_bgImage.width()), ih = qMax(1, m_bgImage.height());
        const qreal scale = qMax(width() / iw, height() / ih); // cover
        const qreal sw = width() / scale, sh = height() / scale;
        m_imageNode->setSourceRect(QRectF((iw - sw) / 2, (ih - sh) / 2, sw, sh));
    }

    // Backgrounds + selection (vertex colour).
    if (!m_bgNode) {
        m_bgNode = new QSGGeometryNode;
        auto *geo = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        geo->setDrawingMode(QSGGeometry::DrawTriangles);
        m_bgNode->setGeometry(geo);
        m_bgNode->setFlag(QSGNode::OwnsGeometry);
        m_bgNode->setMaterial(new QSGVertexColorMaterial);
        m_bgNode->setFlag(QSGNode::OwnsMaterial);
        root->appendChildNode(m_bgNode);
    }
    {
        QSGGeometry *geo = m_bgNode->geometry();
        geo->allocate(int(bg.size()));
        memcpy(geo->vertexData(), bg.data(), bg.size() * sizeof(QSGGeometry::ColoredPoint2D));
        m_bgNode->markDirty(QSGNode::DirtyGeometry);
    }

    // Glyphs (atlas material). The texture (GlyphAtlasTexture, glyphcache.h) is created once and
    // kept for the node's whole life; it re-uploads only the atlas sub-rectangles GlyphCache
    // rasterised since the last frame (see its commitTextureOperations(), called every frame from
    // GlyphShader::updateSampledImage() below) instead of the whole 2048x2048 atlas.
    if (!m_glyphNode) {
        m_glyphNode = new QSGGeometryNode;
        auto *geo = new QSGGeometry(kGlyphAttrSet, 0);
        geo->setDrawingMode(QSGGeometry::DrawTriangles);
        m_glyphNode->setGeometry(geo);
        m_glyphNode->setFlag(QSGNode::OwnsGeometry);
        m_glyphNode->setMaterial(new GlyphMaterial);
        m_glyphNode->setFlag(QSGNode::OwnsMaterial);
        root->appendChildNode(m_glyphNode);
    }
    auto *glyphMat = static_cast<GlyphMaterial *>(m_glyphNode->material());
    if (!glyphMat->texture)
        glyphMat->setTexture(new GlyphAtlasTexture);
    static_cast<GlyphAtlasTexture *>(glyphMat->texture)
        ->addDirtyRects(m_glyphs.atlas(), m_glyphs.takeDirtyRects());
    {
        QSGGeometry *geo = m_glyphNode->geometry();
        geo->allocate(int(glyphs.size()));
        if (!glyphs.empty())
            memcpy(geo->vertexData(), glyphs.data(), glyphs.size() * sizeof(GlyphVertex));
        m_glyphNode->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    }

    // Cursor (vertex colour, on top).
    if (!m_cursorNode) {
        m_cursorNode = new QSGGeometryNode;
        auto *geo = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        geo->setDrawingMode(QSGGeometry::DrawTriangles);
        m_cursorNode->setGeometry(geo);
        m_cursorNode->setFlag(QSGNode::OwnsGeometry);
        m_cursorNode->setMaterial(new QSGVertexColorMaterial);
        m_cursorNode->setFlag(QSGNode::OwnsMaterial);
        root->appendChildNode(m_cursorNode);
    }
    {
        QSGGeometry *geo = m_cursorNode->geometry();
        geo->allocate(int(cursor.size()));
        if (!cursor.empty())
            memcpy(geo->vertexData(), cursor.data(), cursor.size() * sizeof(QSGGeometry::ColoredPoint2D));
        m_cursorNode->markDirty(QSGNode::DirtyGeometry);
    }

    return root;
}

// ─── input ──────────────────────────────────────────────────────────────────────────────────────

void TerminalView::keyToVTerm(QKeyEvent *event)
{
    if (m_tmux) { // tmux encodes keys for the pane's modes and its TERM (3.3)
        tmuxKeyEvent(event);
        return;
    }
    VTermModifier mod = VTERM_MOD_NONE;
    if (event->modifiers() & Qt::ShiftModifier)
        mod = VTermModifier(mod | VTERM_MOD_SHIFT);
    if (event->modifiers() & Qt::ControlModifier)
        mod = VTermModifier(mod | VTERM_MOD_CTRL);
    if (event->modifiers() & Qt::AltModifier)
        mod = VTermModifier(mod | VTERM_MOD_ALT);

    VTermKey key = VTERM_KEY_NONE;
    switch (event->key()) {
    case Qt::Key_Return: case Qt::Key_Enter: key = VTERM_KEY_ENTER; break;
    case Qt::Key_Backspace: key = VTERM_KEY_BACKSPACE; break;
    case Qt::Key_Tab: key = VTERM_KEY_TAB; break;
    case Qt::Key_Escape: key = VTERM_KEY_ESCAPE; break;
    case Qt::Key_Up: key = VTERM_KEY_UP; break;
    case Qt::Key_Down: key = VTERM_KEY_DOWN; break;
    case Qt::Key_Left: key = VTERM_KEY_LEFT; break;
    case Qt::Key_Right: key = VTERM_KEY_RIGHT; break;
    case Qt::Key_Insert: key = VTERM_KEY_INS; break;
    case Qt::Key_Delete: key = VTERM_KEY_DEL; break;
    case Qt::Key_Home: key = VTERM_KEY_HOME; break;
    case Qt::Key_End: key = VTERM_KEY_END; break;
    case Qt::Key_PageUp: key = VTERM_KEY_PAGEUP; break;
    case Qt::Key_PageDown: key = VTERM_KEY_PAGEDOWN; break;
    default:
        if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F12)
            key = VTermKey(VTERM_KEY_FUNCTION(event->key() - Qt::Key_F1 + 1));
        break;
    }

    if (key != VTERM_KEY_NONE) {
        vterm_keyboard_key(m_vt, key, mod);
        return;
    }

    // Ctrl+letter etc.: hand libvterm the PLAIN character + the modifier — it builds the control
    // byte / CSI-u itself. Otherwise send the composed text as-is.
    const QString text = event->text();
    if ((mod & VTERM_MOD_CTRL) && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) {
        vterm_keyboard_unichar(m_vt, uint32_t('a' + (event->key() - Qt::Key_A)), mod);
        return;
    }
    if (!text.isEmpty()) {
        const auto ucs4 = text.toUcs4();
        for (const char32_t c : ucs4)
            vterm_keyboard_unichar(m_vt, c, event->modifiers() & Qt::AltModifier ? VTERM_MOD_ALT : VTERM_MOD_NONE);
    }
}

void TerminalView::inputMethodEvent(QInputMethodEvent *event)
{
    const QString commit = event->commitString();
    // Pre-edit (the pending dead key / IME composition) is not drawn in the grid; only the
    // committed text reaches the shell, exactly as a typed character would.
    if (m_vt && !m_readOnly && !commit.isEmpty()) {
        if (m_scrollOffset > 0) {
            m_scrollOffset = 0;
            emit scrollChanged();
            update();
        }
        resetBlink();
        if (m_tmux) {
            std::string utf8;
            for (const char32_t c : commit.toUcs4())
                if (c >= 0x20 && c != 0x7f)
                    tmux::appendUtf8(utf8, c);
            tmuxQueueLiteral(utf8);
        } else {
            ScreenLock lock(m_screenMutex);
            for (const char32_t c : commit.toUcs4())
                vterm_keyboard_unichar(m_vt, c, VTERM_MOD_NONE);
        }
    }
    event->accept();
}

QVariant TerminalView::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImCursorRectangle: // where an IME candidate popup should appear: the cursor cell
        return QRectF(m_cursor.col * m_cellW, m_cursor.row * m_cellH, m_cellW, m_cellH);
    case Qt::ImHints: // a shell, not prose: no auto-capitalisation / prediction
        return int(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText | Qt::ImhPreferLatin);
    default:
        return QQuickItem::inputMethodQuery(query);
    }
}

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    if (Q_UNLIKELY(PerfTrace::on)) {
        if (PerfTrace::floodKeys)
            PerfTrace::keyEvent(); // Key was stamped at injection (main.cpp SOLIDTERM_FLOODKEYS)
        else
            PerfTrace::stage(PerfTrace::Key);
    }
    if (!m_vt) {
        QQuickItem::keyPressEvent(event);
        return;
    }
    // Reserved accelerators (split/close/focus … from the solid config) are consumed HERE, before
    // the pty — so they never leak to the shell and never hit Qt's ambiguous Shortcut map.
    if (!m_reserved.isEmpty()) {
        // Font-zoom chords by KEY CODE, not character: pressing Ctrl+'+' is really Ctrl+Shift+'=' (and
        // on non-US layouts the '='/'−'/'0' characters shift around), so a PortableText match misses
        // them. Normalise Ctrl+(Plus|Equal)→"Ctrl+=", Ctrl+(Minus|Underscore)→"Ctrl+-", Ctrl+0→"Ctrl+0"
        // regardless of Shift/layout, then dispatch through the same reserved-accelerator path.
        if (event->modifiers() & Qt::ControlModifier) {
            QString norm;
            switch (event->key()) {
            case Qt::Key_Plus: case Qt::Key_Equal:       norm = QStringLiteral("Ctrl+="); break;
            case Qt::Key_Minus: case Qt::Key_Underscore: norm = QStringLiteral("Ctrl+-"); break;
            case Qt::Key_0:                              norm = QStringLiteral("Ctrl+0"); break;
            }
            if (!norm.isEmpty() && m_reserved.contains(norm)) {
                emit accelerator(norm);
                event->accept();
                return;
            }
        }
        const QString seq = QKeySequence(event->keyCombination()).toString(QKeySequence::PortableText);
        if (!seq.isEmpty() && m_reserved.contains(seq)) {
            emit accelerator(seq);
            event->accept();
            return;
        }
    }
    // Copy/paste (Ctrl+Shift+C/V — the terminal convention; plain Ctrl+C/V belong to the shell).
    if ((event->modifiers() & Qt::ControlModifier) && (event->modifiers() & Qt::ShiftModifier)) {
        if (event->key() == Qt::Key_V) {
            pasteClipboard();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_C && m_selValid) {
            copySelection();
            event->accept();
            return;
        }
    }
    // Shift+Insert pastes the clipboard (the other terminal convention).
    if ((event->modifiers() & Qt::ShiftModifier) && event->key() == Qt::Key_Insert) {
        pasteClipboard();
        event->accept();
        return;
    }
    // Typing snaps back to live output.
    if (m_scrollOffset > 0) {
        m_scrollOffset = 0;
        emit scrollChanged();
        update();
    }
    if (m_readOnly) { // read-only pane: consume the key but don't feed the pty
        event->accept();
        return;
    }
    resetBlink();
    {
        ScreenLock lock(m_screenMutex); // libvterm's encoder reads its modes and fills its output buffer
        keyToVTerm(event);
    }
    event->accept();
}

TerminalView::CellPos TerminalView::cellAt(const QPointF &p) const
{
    CellPos c;
    const int row = qBound(0, int(p.y() / m_cellH), m_rows - 1);
    {
        ScreenLock lock(m_screenMutex);
        c.line = topLineLocked() + row;
    }
    c.col = qBound(0, int(p.x() / m_cellW), m_cols - 1);
    return c;
}

void TerminalView::selectedRange(CellPos &from, CellPos &to) const
{
    const bool fwd = !lineBefore(m_selEnd, m_selAnchor);
    from = fwd ? m_selAnchor : m_selEnd;
    to = fwd ? m_selEnd : m_selAnchor;
}

QString TerminalView::selectedText() const
{
    if (!m_selValid)
        return QString();
    CellPos from, to;
    selectedRange(from, to);
    QString out;
    ScreenLock lock(m_screenMutex);
    const uint64_t base = m_term.lineBase();
    for (LineNo ln = std::max(from.line, oldestLine(base)); ln <= to.line; ++ln) {
        const int c0 = ln == from.line ? from.col : 0;
        const int c1 = ln == to.line ? to.col : m_cols - 1;
        QString line;
        const TermScreen::Line src = m_term.line(int(absRowOfLine(base, ln)));
        for (int col = c0; col <= c1; ++col) {
            if (!src.cells) // past the live screen
                break;
            VTermScreenCell cell;
            m_term.cell(src, col, &cell);
            if (cell.chars[0] == 0) {
                line.append(QLatin1Char(' '));
            } else {
                for (int ci = 0; ci < VTERM_MAX_CHARS_PER_CELL && cell.chars[ci]; ++ci)
                    line.append(QString::fromUcs4(&cell.chars[ci], 1));
            }
            if (cell.width > 1)
                col += cell.width - 1;
        }
        while (line.endsWith(QLatin1Char(' ')))
            line.chop(1);
        out += line;
        if (ln != to.line)
            out += QLatin1Char('\n');
    }
    return out;
}

int TerminalView::debugSelectionRow() const
{
    if (!m_selValid)
        return -1000000;
    CellPos from, to;
    selectedRange(from, to);
    ScreenLock lock(m_screenMutex);
    return int(viewRowOfLine(topLineLocked(), from.line));
}

int TerminalView::debugFindRow(const QString &needle) const
{
    ScreenLock lock(m_screenMutex);
    const LineNo top = topLineLocked();
    const uint64_t base = m_term.lineBase();
    for (int row = m_rows - 1; row >= 0; --row) {
        if (rowText(int(absRowOfLine(base, top + row))).contains(needle))
            return row;
    }
    return -1;
}

void TerminalView::copySelection()
{
    const QString text = selectedText();
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

// "Copy link" / "Open link" (right-click context menu, Solid-side — see term.tsx): act on whatever
// link is CURRENTLY hovered. The menu only offers these when hoverLinkUrl() is non-empty (see
// hoverLinkChanged), so by the time either runs m_hoverLink should still be valid — but the mouse
// could in principle have moved between the right-click and the menu action, so both no-op quietly
// on an empty/stale hover rather than acting on the wrong link.
void TerminalView::copyHoverLink()
{
    if (!m_hoverLink.valid)
        return;
    QGuiApplication::clipboard()->setText(m_hoverLink.url);
    QGuiApplication::clipboard()->setText(m_hoverLink.url, QClipboard::Selection);
}

void TerminalView::openHoverLink()
{
    if (m_hoverLink.valid)
        QDesktopServices::openUrl(QUrl::fromUserInput(m_hoverLink.url));
}

void TerminalView::pasteClipboard()
{
    const QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty())
        return;
    // Bracketed-paste-worthy safety: multiline text (a stray newline runs the command) → confirm
    // in the Solid dialog first; single-line pastes go straight through.
    if (text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r')))
        emit unsafePasteRequested(text);
    else
        pasteText(text);
}

void TerminalView::pasteText(const QString &text)
{
    if (m_tmux)
        pasteBracketed(text);
    else
        sendText(text); // confirmed (or explicitly-chosen) paste — no re-prompt
}

void TerminalView::clearScrollback()
{
    {
        ScreenLock lock(m_screenMutex);
        m_term.clearScrollback();
        m_seenPushed = m_term.pushedTotal();
    }
    m_sbCount = 0;
    m_scrollOffset = 0;
    m_selValid = false;
    dropEvictedAnchors(m_term.lineBase()); // search matches in the scrollback are gone too
    emit selectionChanged();
    emit scrollChanged();
    update();
}

void TerminalView::setDimmed(bool v)
{
    if (m_dimmed == v)
        return;
    m_dimmed = v;
    update();
}

void TerminalView::setReadOnly(bool v)
{
    if (m_readOnly == v)
        return;
    m_readOnly = v;
    emit readOnlyChanged();
}

void TerminalView::resetTerminal()
{
    if (m_term.isAttached()) {
        {
            ScreenLock lock(m_screenMutex);
            m_term.reset(true); // hard reset: clears attrs, modes, cursor
        }
        applyNews();
    }
    if (m_selValid) { // the screen it pointed into was just erased
        m_selValid = false;
        emit selectionChanged();
    }
    m_scrollOffset = 0;
    emit scrollChanged();
    update();
}

void TerminalView::resetBlink()
{
    m_blinkOn = true;
    if (m_blinkTimer)
        m_blinkTimer->start(); // restart the phase so the cursor stays solid right after activity
    update();
}

// DECSET 1004: a program (tmux, vim) that asked for focus events gets a report whenever THIS pane
// both has Qt's active focus AND its window is the active one — the closest native analogue of
// "the terminal application is focused". vterm_state_focus_in/out() decide internally whether the
// program actually asked for 1004; we just report the transition, unconditionally, like xterm does.
void TerminalView::updateFocusReport()
{
    if (!m_vt)
        return;
    const bool focused = hasActiveFocus() && window() && window()->isActive();
    if (focused == m_focusReported)
        return;
    m_focusReported = focused;
    ScreenLock lock(m_screenMutex);
    VTermState *state = vterm_obtain_state(m_vt);
    if (focused)
        vterm_state_focus_in(state);
    else
        vterm_state_focus_out(state);
}

// ─── search ─────────────────────────────────────────────────────────────────────────────────────

QString TerminalView::rowText(int absRow) const
{
    QString line(m_cols, QLatin1Char(' '));
    if (!m_term.isAttached() || absRow < 0)
        return line;
    const TermScreen::Line src = m_term.line(absRow); // empty past the live screen
    for (int c = 0; c < m_cols && c < src.cols; ++c) {
        // One UTF-16 unit per column: a code point outside the BMP, and the right half of a wide
        // glyph (kWideContinuation = 0xFFFFFFFF), become U+FFFD — QChar(char32_t) asserts on both in
        // a debug build and truncates them to arbitrary BMP characters in a release one.
        const char32_t ch = src.cells[c].chars[0];
        if (ch)
            line[c] = ch <= 0xffff ? QChar(char16_t(ch)) : QChar(QChar::ReplacementCharacter);
    }
    return line;
}

void TerminalView::search(const QString &query)
{
    applyNews(); // absolute rows below agree with the GUI's scrollback count
    m_searchQuery = query;
    m_matches.clear();
    m_searchIndex = -1;
    if (!query.isEmpty() && m_term.isAttached()) {
        ScreenLock lock(m_screenMutex); // the whole walk: the parser thread waits (user action, once)
        const int total = m_term.sbCount() + m_rows;
        const uint64_t base = m_term.lineBase();
        for (int absRow = 0; absRow < total; ++absRow) {
            const QString line = rowText(absRow);
            int from = 0;
            while (true) {
                const int idx = line.indexOf(query, from, Qt::CaseInsensitive);
                if (idx < 0)
                    break;
                m_matches.append({ lineOfAbsRow(base, absRow), idx, int(query.length()) });
                from = idx + query.length();
            }
        }
    }
    if (!m_matches.isEmpty()) {
        m_searchIndex = m_matches.size() - 1; // most recent (nearest the bottom)
        scrollToMatch(m_searchIndex);
    }
    emit searchChanged(m_searchIndex + 1, int(m_matches.size()));
    update();
}

void TerminalView::scrollToMatch(int i)
{
    if (i < 0 || i >= m_matches.size())
        return;
    const int sbSize = m_sbCount;
    const int64_t desiredTop = absRowOfLine(m_lineBase, m_matches[i].line) - m_rows / 2; // centre the match
    m_scrollOffset = int(std::clamp<int64_t>(sbSize - desiredTop, 0, sbSize));
    emit scrollChanged();
}

void TerminalView::searchNext()
{
    if (m_matches.isEmpty())
        return;
    m_searchIndex = (m_searchIndex + 1) % m_matches.size();
    scrollToMatch(m_searchIndex);
    emit searchChanged(m_searchIndex + 1, int(m_matches.size()));
    update();
}

void TerminalView::searchPrev()
{
    if (m_matches.isEmpty())
        return;
    m_searchIndex = (m_searchIndex - 1 + m_matches.size()) % m_matches.size();
    scrollToMatch(m_searchIndex);
    emit searchChanged(m_searchIndex + 1, int(m_matches.size()));
    update();
}

void TerminalView::clearSearch()
{
    if (m_searchQuery.isEmpty() && m_matches.isEmpty())
        return;
    m_searchQuery.clear();
    m_matches.clear();
    m_searchIndex = -1;
    emit searchChanged(0, 0);
    update();
}

// ─── hyperlinks ───────────────────────────────────────────────────────────────────────────────

// Soft-wrap join cap: a URL can legitimately run this many terminal rows before we give up growing
// it (good for very long URLs, bounds the cost of a hover/click — VTermLineInfo::continuation only
// exists for the LIVE screen, so a scrollback row never joins; see the file-level note in linkscan.h).
namespace { constexpr int kLinkJoinRowCap = 6; }

TerminalView::LinkSpan TerminalView::linkAt(const QPointF &p) const
{
    if (!m_term.isAttached())
        return {};
    ScreenLock lock(m_screenMutex); // rows + libvterm's line info (reallocated by a resize)
    const int row = int(p.y() / m_cellH);
    const int col = int(p.x() / m_cellW);
    if (row < 0 || row >= m_rows || col < 0 || col >= m_cols)
        return {};

    // The screen as it is now (the parser may be ahead of the GUI's copies): same anchoring rule
    // as the renderer.
    const int sbSize = m_term.sbCount();
    const int queriedAbsRow = sbSize - clampOffset(effectiveOffset(sbSize, m_term.pushedTotal()), sbSize) + row;
    const int liveRow = queriedAbsRow - sbSize; // >= 0 while this row is the live screen, not scrollback

    // Find the actual start of the (possibly wrapped) line: walk backward over LIVE rows flagged as
    // a continuation of the previous one. Scrollback rows never join (no VTermLineInfo there).
    int startAbsRow = queriedAbsRow;
    if (liveRow >= 0 && m_vt) {
        VTermState *state = vterm_obtain_state(m_vt);
        int steps = 0;
        int lr = liveRow;
        while (lr > 0 && steps < kLinkJoinRowCap) {
            const VTermLineInfo *info = vterm_state_get_lineinfo(state, lr);
            if (!info || !info->continuation)
                break;
            --lr;
            ++steps;
        }
        startAbsRow = queriedAbsRow - (liveRow - lr);
    }

    // Join forward from startAbsRow while each NEXT row is itself a continuation, capped. Every
    // row contributes exactly m_cols characters, so position <-> (row, col) is a simple divmod.
    QString joined;
    int rows = 1;
    joined += rowText(startAbsRow);
    if (liveRow >= 0 && m_vt) {
        VTermState *state = vterm_obtain_state(m_vt);
        int lr = startAbsRow - sbSize + 1;
        while (rows < kLinkJoinRowCap && lr < m_rows) {
            const VTermLineInfo *info = vterm_state_get_lineinfo(state, lr);
            if (!info || !info->continuation)
                break;
            joined += rowText(startAbsRow + rows);
            ++rows;
            ++lr;
        }
    }

    // ASCII-lossy on purpose (matches the renderer's own assumption — see rowText callers elsewhere):
    // a non-Latin1 cell becomes '?', which can never start or extend a scheme match.
    const std::string scan = joined.toLatin1().toStdString();
    const int queriedIdx = (queriedAbsRow - startAbsRow) * m_cols + col;
    for (const LinkMatch &m : scanLinks(scan)) {
        const int mStart = int(m.start), mEnd = int(m.end);
        if (queriedIdx < mStart || queriedIdx >= mEnd)
            continue;
        const int rowStartIdx = (queriedAbsRow - startAbsRow) * m_cols;
        const int rowEndIdx = rowStartIdx + m_cols;
        LinkSpan s;
        s.line = lineOfAbsRow(m_term.lineBase(), queriedAbsRow);
        s.c0 = qMax(mStart, rowStartIdx) - rowStartIdx;
        s.c1 = qMin(mEnd, rowEndIdx) - rowStartIdx - 1;
        s.url = joined.mid(mStart, mEnd - mStart);
        if (s.url.startsWith(QLatin1String("www.")))
            s.url.prepend(QLatin1String("http://"));
        s.valid = true;
        return s;
    }
    return {};
}

void TerminalView::hoverEnterEvent(QHoverEvent *event)
{
    emit hovered();
    hoverMoveEvent(event); // same cursor/link state as any hover position
}

void TerminalView::hoverMoveEvent(QHoverEvent *event)
{
    // MOVE mode (DECSET 1003) reports motion with NO button down too — that only ever reaches this
    // item as a hover (mouseMoveEvent is only delivered between a press and its release).
    const bool isReport = decideMouseAction(m_mouseMode, m_altScreen, event->modifiers() & Qt::ShiftModifier) == MouseAction::Report;
    if (isReport)
        reportMouseMotion(event->position(), event->modifiers(), Qt::NoButton);
    // Links are a LOCAL thing (like selection): with mouse reporting active, they only apply when
    // Shift is held too — decideMouseAction already encodes that rule (Report wins unless Shift).
    const LinkSpan link = isReport ? LinkSpan{} : linkAt(event->position());
    setCursor(link.valid ? Qt::PointingHandCursor : Qt::IBeamCursor);
    if (link.valid != m_hoverLink.valid || link.line != m_hoverLink.line
        || link.c0 != m_hoverLink.c0 || link.c1 != m_hoverLink.c1) {
        m_hoverLink = link;
        emit hoverLinkChanged(link.valid ? link.url : QString());
        update();
    }
}

void TerminalView::hoverLeaveEvent(QHoverEvent *)
{
    unsetCursor();
    if (m_hoverLink.valid) {
        m_hoverLink = {};
        emit hoverLinkChanged(QString());
        update();
    }
}

// ─── mouse ────────────────────────────────────────────────────────────────────────────────────

namespace {
// Qt's modifier bits → libvterm's, for mouse reports (keyToVTerm does the same for the keyboard).
VTermModifier qtModsToVTerm(Qt::KeyboardModifiers m)
{
    VTermModifier mod = VTERM_MOD_NONE;
    if (m & Qt::ShiftModifier) mod = VTermModifier(mod | VTERM_MOD_SHIFT);
    if (m & Qt::ControlModifier) mod = VTermModifier(mod | VTERM_MOD_CTRL);
    if (m & Qt::AltModifier) mod = VTermModifier(mod | VTERM_MOD_ALT);
    return mod;
}

// xterm convention: left/middle/right = 1/2/3 (libvterm encodes the actual X10/SGR byte); 0 = "not
// a button libvterm's mouse API knows about" (e.g. a fourth/back/forward mouse button).
int vtermButtonFor(Qt::MouseButton b)
{
    switch (b) {
    case Qt::LeftButton: return 1;
    case Qt::MiddleButton: return 2;
    case Qt::RightButton: return 3;
    default: return 0;
    }
}
} // namespace

bool TerminalView::wantsMouseReport(int qtModifiers) const
{
    const bool shift = Qt::KeyboardModifiers(qtModifiers) & Qt::ShiftModifier;
    return decideMouseAction(m_mouseMode, m_altScreen, shift) == MouseAction::Report;
}

// xterm never reports a scrolled-back position: any reported event first snaps the view back to
// live, exactly like typing already does (see keyPressEvent).
void TerminalView::snapToLive()
{
    if (m_scrollOffset > 0) {
        m_scrollOffset = 0;
        emit scrollChanged();
        update();
    }
}

void TerminalView::reportMousePress(Qt::MouseButton button, const QPointF &pos, Qt::KeyboardModifiers mods)
{
    const int vb = vtermButtonFor(button);
    if (!m_vt || vb == 0)
        return;
    snapToLive();
    const VTermModifier mod = qtModsToVTerm(mods);
    const int col = pixelToCellIndex(pos.x(), m_cellW, m_cols);
    const int row = pixelToCellIndex(pos.y(), m_cellH, m_rows);
    ScreenLock lock(m_screenMutex);
    vterm_mouse_move(m_vt, row, col, mod);
    vterm_mouse_button(m_vt, vb, true, mod);
    m_lastReportedCell = { row, col };
}

void TerminalView::reportMouseRelease(Qt::MouseButton button, const QPointF &pos, Qt::KeyboardModifiers mods)
{
    const int vb = vtermButtonFor(button);
    if (!m_vt || vb == 0)
        return;
    const VTermModifier mod = qtModsToVTerm(mods);
    const int col = pixelToCellIndex(pos.x(), m_cellW, m_cols);
    const int row = pixelToCellIndex(pos.y(), m_cellH, m_rows);
    ScreenLock lock(m_screenMutex);
    vterm_mouse_move(m_vt, row, col, mod);
    vterm_mouse_button(m_vt, vb, false, mod);
    m_lastReportedCell = { row, col };
}

// Motion only reports in DRAG mode while a button is down, or unconditionally in MOVE mode (CLICK
// mode never reports motion at all — libvterm itself enforces that from m_mouseMode); either way,
// only on an actual cell change, so a touchpad doesn't flood the pty with same-cell reports.
void TerminalView::reportMouseMotion(const QPointF &pos, Qt::KeyboardModifiers mods, Qt::MouseButtons buttons)
{
    if (!m_vt)
        return;
    const bool wants = m_mouseMode == VTERM_PROP_MOUSE_MOVE
        || (m_mouseMode == VTERM_PROP_MOUSE_DRAG && buttons != Qt::NoButton);
    if (!wants)
        return;
    const int col = pixelToCellIndex(pos.x(), m_cellW, m_cols);
    const int row = pixelToCellIndex(pos.y(), m_cellH, m_rows);
    if (row == m_lastReportedCell.row && col == m_lastReportedCell.col)
        return;
    m_lastReportedCell = { row, col };
    ScreenLock lock(m_screenMutex);
    vterm_mouse_move(m_vt, row, col, qtModsToVTerm(mods));
}

// Wheel → xterm buttons 4 (up) / 5 (down), one PRESS per physical notch (no matching release — the
// wheel protocol only ever sends the press byte).
void TerminalView::reportWheel(const QPointF &pos, int angleDeltaY, Qt::KeyboardModifiers mods)
{
    const int notches = accumulateWheelNotches(m_wheelPending, angleDeltaY);
    if (!m_vt || notches == 0)
        return;
    snapToLive();
    const VTermModifier mod = qtModsToVTerm(mods);
    const int col = pixelToCellIndex(pos.x(), m_cellW, m_cols);
    const int row = pixelToCellIndex(pos.y(), m_cellH, m_rows);
    ScreenLock lock(m_screenMutex);
    vterm_mouse_move(m_vt, row, col, mod);
    const int button = notches > 0 ? 4 : 5;
    for (int i = 0, n = qAbs(notches); i < n; ++i)
        vterm_mouse_button(m_vt, button, true, mod);
}

// Alternate-scroll (no mouse mode, altscreen app: less/man/git log): the wheel becomes cursor
// Up/Down keys, 3 per notch — the same "3 lines" feel as the ordinary scrollback wheel, and through
// vterm_keyboard_key so DECCKM (application cursor-key mode) picks the right escape (\eOA vs \e[A).
void TerminalView::sendAltScrollKeys(int angleDeltaY)
{
    const int notches = accumulateWheelNotches(m_wheelPending, angleDeltaY);
    if (!m_vt || notches == 0)
        return;
    const VTermKey key = notches > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN;
    ScreenLock lock(m_screenMutex);
    for (int i = 0, n = qAbs(notches) * 3; i < n; ++i)
        vterm_keyboard_key(m_vt, key, VTERM_MOD_NONE);
}

void TerminalView::selectWordAt(const CellPos &c)
{
    QString line;
    {
        ScreenLock lock(m_screenMutex);
        line = rowText(int(absRowOfLine(m_term.lineBase(), c.line)));
    }
    const auto isWord = [](QChar ch) {
        return ch.isLetterOrNumber() || QStringLiteral("._-~/:@%+=").contains(ch);
    };
    if (c.col >= line.size() || !isWord(line[c.col]))
        return;
    int a = c.col, b = c.col;
    while (a > 0 && isWord(line[a - 1])) --a;
    while (b + 1 < line.size() && isWord(line[b + 1])) ++b;
    m_selAnchor = { c.line, a };
    m_selEnd = { c.line, b };
    m_selValid = true;
    emit selectionChanged();
    update();
}

void TerminalView::selectLineAt(const CellPos &c)
{
    QString line;
    {
        ScreenLock lock(m_screenMutex);
        line = rowText(int(absRowOfLine(m_term.lineBase(), c.line)));
    }
    int end = line.size() - 1;
    while (end > 0 && line[end] == QLatin1Char(' ')) --end;
    m_selAnchor = { c.line, 0 };
    m_selEnd = { c.line, qMax(0, end) };
    m_selValid = true;
    emit selectionChanged();
    update();
}

void TerminalView::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Local only (same rule as selection/links generally — see hoverMoveEvent): with mouse reporting
    // active, this double-click would already have gone through mousePressEvent's Report branch for
    // BOTH of the two presses that make up a double-click, so reaching here at all already implies
    // Local — still, be explicit rather than relying on that.
    const bool isReport = decideMouseAction(m_mouseMode, m_altScreen, event->modifiers() & Qt::ShiftModifier) == MouseAction::Report;
    const LinkSpan link = isReport ? LinkSpan{} : linkAt(event->position());
    if (link.valid) {
        // The whole URL, not a "word" — VTE/GNOME Terminal convention: double-click on a link
        // selects it entirely, ignoring selectWordAt's narrower word-separator rules (/:.?&=# etc.
        // are all valid URL characters that a plain word-select would stop at).
        m_selAnchor = { link.line, link.c0 };
        m_selEnd = { link.line, link.c1 };
        m_selValid = true;
        emit selectionChanged();
        update();
    } else {
        selectWordAt(cellAt(event->position())); // double-click → select the word
    }
    m_lastDblTime = event->timestamp();
    m_selecting = false;
    resumeOutputFromSelection(); // the first press of the pair already paused output; the pair's own
                                 // mouseReleaseEvent never runs (this handler replaces it) — resume here
    event->accept();
}

void TerminalView::mousePressEvent(QMouseEvent *event)
{
    applyNews(); // positions below (cellAt) use the GUI's copies: bring them up to the screen
    forceActiveFocus(Qt::MouseFocusReason);
    emit focusRequested(); // user intent: this pane is now the focused one (TerminalPanes updates m_focused)

    // A program that turned mouse tracking on gets the click reported (xterm/VTE behaviour) instead
    // of any of TerminalView's own button handling below — no paste, no selection, no local menu.
    // Right only ever reaches here via the QML <ContextMenu> declining the press (wantsMouseReport),
    // so this is always true for Right; Left/Middle can go either way.
    if (decideMouseAction(m_mouseMode, m_altScreen, event->modifiers() & Qt::ShiftModifier) == MouseAction::Report) {
        reportMousePress(event->button(), event->position(), event->modifiers());
        event->accept();
        return;
    }
    stopAutoscroll(); // a fresh press always starts clean, even if a previous drag leaked one somehow
    resumeOutputFromSelection(); // ditto for a leaked pause (no-op when not paused)

    // Middle-click pastes the PRIMARY selection (X11/Wayland convention).
    if (event->button() == Qt::MiddleButton) {
        const QString sel = QGuiApplication::clipboard()->text(QClipboard::Selection);
        if (!sel.isEmpty()) {
            if (sel.contains(QLatin1Char('\n')) || sel.contains(QLatin1Char('\r')))
                emit unsafePasteRequested(sel);
            else
                sendText(sel);
        }
        event->accept();
        return;
    }
    // Triple-click (a press shortly after a double-click) → select the whole line.
    if (event->button() == Qt::LeftButton && event->timestamp() - m_lastDblTime < 500) {
        m_lastDblTime = 0;
        selectLineAt(cellAt(event->position()));
        event->accept();
        return;
    }
    // Ctrl+click on an auto-detected link opens it (before starting a selection).
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ControlModifier)) {
        const LinkSpan link = linkAt(event->position());
        if (link.valid) {
            QDesktopServices::openUrl(QUrl::fromUserInput(link.url));
            event->accept();
            return;
        }
    }
    if (event->button() == Qt::LeftButton) {
        m_selAnchor = m_selEnd = cellAt(event->position());
        m_selecting = true;
        pauseOutputForSelection(); // STOP reading the pty for as long as this drag lasts
        if (m_selValid) {
            m_selValid = false;
            emit selectionChanged();
        }
        update();
    }
    event->accept();
}

void TerminalView::mouseMoveEvent(QMouseEvent *event)
{
    if (decideMouseAction(m_mouseMode, m_altScreen, event->modifiers() & Qt::ShiftModifier) == MouseAction::Report) {
        stopAutoscroll(); // defence in depth: a mode/shift flip mid-drag must not leave it running
        reportMouseMotion(event->position(), event->modifiers(), event->buttons());
        event->accept();
        return;
    }
    if (!m_selecting)
        return;
    // cellAt() clamps the row to [0, m_rows-1], so a pointer already past the edge extends the
    // selection to the currently-visible edge row immediately; updateAutoscroll (below) is what then
    // keeps revealing MORE of the scrollback/live screen without further movement.
    const CellPos p = cellAt(event->position());
    if (p != m_selEnd) {
        m_selEnd = p;
        const bool valid = m_selEnd != m_selAnchor;
        if (valid != m_selValid) {
            m_selValid = valid;
            emit selectionChanged();
        }
        update();
    }
    updateAutoscroll(event->position());
    event->accept();
}

void TerminalView::mouseReleaseEvent(QMouseEvent *event)
{
    if (decideMouseAction(m_mouseMode, m_altScreen, event->modifiers() & Qt::ShiftModifier) == MouseAction::Report) {
        reportMouseRelease(event->button(), event->position(), event->modifiers());
        event->accept();
        return;
    }
    m_selecting = false;
    stopAutoscroll();
    resumeOutputFromSelection(); // drag is over — resume pty reads and drain whatever queued up
    // Copy-on-select to the PRIMARY selection (middle-click paste; doesn't clobber the clipboard).
    if (m_selValid) {
        const QString text = selectedText();
        if (!text.isEmpty())
            QGuiApplication::clipboard()->setText(text, QClipboard::Selection);
    }
    event->accept();
}

// Involuntary grab loss (e.g. an ancestor Flickable/ScrollView deciding to steal the mouse mid-drag):
// the only way a local selection drag can end WITHOUT mouseReleaseEvent ever firing, so this is the
// backstop that still resumes pty output rather than leaving it frozen.
void TerminalView::mouseUngrabEvent()
{
    QQuickItem::mouseUngrabEvent();
    if (m_selecting) {
        m_selecting = false;
        stopAutoscroll();
    }
    resumeOutputFromSelection();
}

// Called from mouseMoveEvent with every fresh LOCAL-selection position: starts the timer the first
// time the pointer sits past an edge, keeps it aimed at the latest position otherwise, and stops it
// the moment the pointer is back inside the view (re-entry — see the file header in autoscroll.h).
void TerminalView::updateAutoscroll(const QPointF &rawPos)
{
    m_autoscrollPos = rawPos;
    if (edgeOvershoot(rawPos.y(), height()) == 0) {
        stopAutoscroll();
        return;
    }
    if (!m_autoscrollTimer) {
        m_autoscrollTimer = new QTimer(this);
        m_autoscrollTimer->setInterval(kAutoscrollTimerMs);
        connect(m_autoscrollTimer, &QTimer::timeout, this, &TerminalView::autoscrollTick);
    }
    if (!m_autoscrollTimer->isActive())
        m_autoscrollTimer->start();
}

void TerminalView::stopAutoscroll()
{
    if (m_autoscrollTimer)
        m_autoscrollTimer->stop();
}

// STOP reading the pty master while the local selection drag this call corresponds to is live (see
// ptysession.h). Idempotent — safe to call speculatively from a defensive reset site.
void TerminalView::pauseOutputForSelection()
{
    if (m_outputPaused)
        return;
    m_outputPaused = true;
    if (m_threaded) {
        {
            ScreenLock lock(m_screenMutex); // once this returns no slice runs until resume
            m_worker.setPaused(true);
        }
        applyNews(); // what was parsed before the pause, so positions agree with the frozen screen
    } else {
        m_pty.pause();
    }
    emit outputPausedChanged();
}

// Resume reading. Safe to call even when not currently paused (every drag-ending site calls this
// unconditionally rather than tracking "did I pause").
void TerminalView::resumeOutputFromSelection()
{
    if (!m_outputPaused)
        return;
    m_outputPaused = false;
    if (m_threaded) {
        ScreenLock lock(m_screenMutex);
        m_worker.setPaused(false);
    } else {
        m_pty.resume();
    }
    emit outputPausedChanged();
}

// One tick: scroll toward whichever edge the LAST known pointer position (m_autoscrollPos) sits past,
// then re-pin the selection's moving end to that edge row at the NEW scroll offset — exactly what a
// real mouseMoveEvent would have done had the content scrolled under a stationary pointer. No fresh
// QMouseEvent is needed for this to keep going, by design (see autoscroll.h).
void TerminalView::autoscrollTick()
{
    const qreal overshoot = edgeOvershoot(m_autoscrollPos.y(), height());
    if (!m_selecting || overshoot == 0) { // pointer re-entered or selection ended between ticks
        stopAutoscroll();
        return;
    }
    const int lines = autoscrollLinesPerTick(qAbs(overshoot), m_cellH);
    if (lines <= 0)
        return;
    const bool above = overshoot < 0;
    m_scrollOffset = autoscrollOffsetAfterTick(m_scrollOffset, m_sbCount, lines, above);
    emit scrollChanged();

    const int edgeRow = above ? 0 : m_rows - 1;
    const CellPos edge = cellAt(QPointF(m_autoscrollPos.x(), edgeRow * m_cellH));
    if (edge != m_selEnd) {
        m_selEnd = edge;
        const bool valid = m_selEnd != m_selAnchor;
        if (valid != m_selValid) {
            m_selValid = valid;
            emit selectionChanged();
        }
    }
    update();
}

void TerminalView::wheelEvent(QWheelEvent *event)
{
    // Ctrl+wheel zooms the font (like Ctrl +/−), instead of scrolling or reporting — takes
    // precedence over everything else, exactly as today.
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->angleDelta().y() != 0)
            emit zoomRequested(event->angleDelta().y() > 0 ? 1 : -1);
        event->accept();
        return;
    }

    const bool shift = event->modifiers() & Qt::ShiftModifier;
    switch (decideMouseAction(m_mouseMode, m_altScreen, shift)) {
    case MouseAction::Report:
        reportWheel(event->position(), event->angleDelta().y(), event->modifiers());
        event->accept();
        return;
    case MouseAction::AltScroll:
        sendAltScrollKeys(event->angleDelta().y());
        event->accept();
        return;
    case MouseAction::Local:
        break;
    }

    // Local (today's behaviour): scroll SolidTerm's own scrollback.
    const int lines = event->angleDelta().y() / 40; // 3 lines per notch
    m_scrollOffset = qBound(0, m_scrollOffset + lines, m_sbCount);
    emit scrollChanged();
    update();
    event->accept();
}

// ─── property surface ───────────────────────────────────────────────────────────────────────────

void TerminalView::setBackgroundImage(const QString &path)
{
    if (m_bgImagePath == path)
        return;
    m_bgImagePath = path;
    m_bgImage = QImage();
    if (!path.isEmpty()) {
        QString p = path;
        if (p.startsWith(QLatin1String("file://")))
            p = QUrl(p).toLocalFile();
        m_bgImageSource.load(p);
    } else {
        m_bgImageSource = QImage();
    }
    rebuildBackgroundImage();
    emit decorChanged();
    update();
}

namespace {

// One box-blur pass of radius r along rows (horizontal) or columns, with a sliding sum — O(pixels),
// independent of r. Premultiplied ARGB, so translucent pixels do not bleed dark fringes.
void boxBlurPass(QImage &img, int r, bool horizontal)
{
    const int w = img.width(), h = img.height();
    const int len = horizontal ? w : h, lines = horizontal ? h : w;
    const qsizetype stride = img.bytesPerLine() / 4;
    QRgb *base = reinterpret_cast<QRgb *>(img.bits());
    const qsizetype step = horizontal ? 1 : stride;
    std::vector<QRgb> line(static_cast<size_t>(len));
    const int win = 2 * r + 1;
    for (int l = 0; l < lines; ++l) {
        QRgb *p = horizontal ? base + l * stride : base + l;
        for (int i = 0; i < len; ++i)
            line[size_t(i)] = p[i * step];
        // Edge pixels are repeated (clamp), so the border does not darken.
        int sa = 0, sr = 0, sg = 0, sb = 0;
        for (int i = -r; i <= r; ++i) {
            const QRgb c = line[size_t(qBound(0, i, len - 1))];
            sa += qAlpha(c); sr += qRed(c); sg += qGreen(c); sb += qBlue(c);
        }
        for (int i = 0; i < len; ++i) {
            p[i * step] = qRgba(sr / win, sg / win, sb / win, sa / win);
            const QRgb out = line[size_t(qBound(0, i - r, len - 1))];
            const QRgb in = line[size_t(qBound(0, i + r + 1, len - 1))];
            sa += qAlpha(in) - qAlpha(out); sr += qRed(in) - qRed(out);
            sg += qGreen(in) - qGreen(out); sb += qBlue(in) - qBlue(out);
        }
    }
}

} // namespace

// Blur over the BACKGROUND image: three box passes each way ≈ a gaussian. Applied ONCE per change
// (not per frame) and never to the glyphs, so text stays crisp. The image is first reduced — a blur
// discards detail anyway, so a smaller texture loses nothing and keeps a large radius cheap; the
// cover-fit draw scales it back up smoothly.
static QImage blurImage(const QImage &src, int strength)
{
    const qreal k = qBound(0, strength, 100) / 100.0;
    constexpr int kWorkSize = 960;   // longest side the blur runs at
    constexpr qreal kMaxRadius = 32; // at full strength, in work-size pixels (~3% of the width)
    QImage img = src.scaled(QSize(kWorkSize, kWorkSize), Qt::KeepAspectRatio, Qt::SmoothTransformation)
                     .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int r = qMax(1, int(kMaxRadius * k));
    for (int pass = 0; pass < 3; ++pass) {
        boxBlurPass(img, r, true);
        boxBlurPass(img, r, false);
    }
    return img;
}

void TerminalView::rebuildBackgroundImage()
{
    m_bgImage = (m_blur > 0 && !m_bgImageSource.isNull())
        ? blurImage(m_bgImageSource, m_blur)
        : m_bgImageSource;
    m_bgImageDirty = true;
}

void TerminalView::setBackgroundOpacity(qreal v)
{
    v = qBound(0.0, v, 1.0);
    if (qFuzzyCompare(m_bgOpacity, v))
        return;
    m_bgOpacity = v;
    emit decorChanged();
    update();
}

void TerminalView::setBlur(int v)
{
    v = qBound(0, v, 100);
    if (m_blur == v)
        return;
    m_blur = v;
    rebuildBackgroundImage(); // the effect lives on the background image, not on the glyphs
    emit decorChanged();
    update();
}

void TerminalView::setScrollbarMode(const QString &v)
{
    if (m_scrollbarMode == v)
        return;
    m_scrollbarMode = v;
    emit scrollbarModeChanged();
}

void TerminalView::scrollTo(int offset)
{
    const int clamped = clampOffset(offset, m_sbCount);
    if (clamped == m_scrollOffset)
        return;
    m_scrollOffset = clamped;
    emit scrollChanged();
    update();
}

// Composes the desktop scrollbar — same look as QmlCss::composeScrollBar (vendor/qml-css-engine's
// cssscroll.h: 12px track, 6px rounded thumb, the same three alpha-black thumb states) but bound to
// scrollOffset/scrollbackCount/viewportRows instead of a Flickable's contentY/contentHeight, since
// TerminalView paints its own grid rather than hosting content in one — "a thin custom bar styled
// from the same CSS hooks" (owner), not a reuse of composeScrollBar itself (its required `flick`
// interface — contentY, contentHeight, visibleArea.heightRatio — has no TerminalView equivalent).
// Lazy + idempotent: called from ensureStarted(), which only has a QQmlEngine once this item is
// actually in a QML scene (C++-created panes go through TerminalPanes' QQmlComponent, so this is
// never null in practice — see TerminalPanes::makeLeaf).
void TerminalView::composeScrollBar()
{
    if (m_scrollBar)
        return;
    QQmlEngine *eng = qmlEngine(this);
    if (!eng)
        return;
    QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("solidterm-scroll-bar"), R"(
import QtQuick
Rectangle {
    id: sbTrack
    required property var view
    objectName: "termScrollTrack"
    z: 1000
    x: view.width - width
    y: 0
    width: 12
    height: view.height
    color: "transparent"
    readonly property int sbSize: view.scrollbackCount
    readonly property int rowCount: view.viewportRows
    readonly property bool hasScrollback: sbSize > 0 && !view.altScreen
    property bool dragging: false
    property bool hoveredBar: false
    property bool pulseOn: false
    function pulse() { pulseOn = true; fadeTimer.restart() }
    Timer { id: fadeTimer; interval: 1000; onTriggered: sbTrack.pulseOn = false }
    Connections { target: view; function onScrollChanged() { sbTrack.pulse() } }
    readonly property bool shown: view.scrollbarMode === "always" ? true
        : view.scrollbarMode === "never" ? false
        : (pulseOn || dragging || hoveredBar || view.outputPaused)
    // A paused drag always shows the bar — even with no scrollback yet — so there's some minimal,
    // theme-able feedback that output is frozen (owner: optional "subtle paused tint").
    visible: (hasScrollback && opacity > 0.01) || view.outputPaused
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 180 } }
    HoverHandler {
        onHoveredChanged: { sbTrack.hoveredBar = hovered; if (hovered) sbTrack.pulse() }
    }
    WheelHandler {
        onWheel: (ev) => view.scrollTo(view.scrollOffset + Math.round(ev.angleDelta.y / 40))
    }
    MouseArea {
        anchors.fill: parent
        onPressed: (m) => {
            sbTrack.pulse()
            var page = Math.max(1, Math.round(sbTrack.rowCount * 0.9))
            if (m.y < sbThumb.y) view.scrollTo(view.scrollOffset + page)
            else if (m.y > sbThumb.y + sbThumb.height) view.scrollTo(view.scrollOffset - page)
        }
    }
    Rectangle {
        id: sbThumb
        objectName: "termScrollThumb"
        x: 3
        width: 6
        radius: 3
        // Paused tint (amber) wins over the normal hover/pressed grey states — minimal, theme-able
        // feedback that a local selection drag has frozen pty reads (owner: subtle, not alarming).
        color: view.outputPaused ? "#99f59e0b"
            : thumbArea.pressed ? "#99000000" : (thumbArea.containsMouse ? "#77000000" : "#55000000")
        height: Math.min(sbTrack.height, Math.max(24,
            (sbTrack.rowCount / Math.max(1, sbTrack.sbSize + sbTrack.rowCount)) * sbTrack.height))
        y: {
            if (sbTrack.sbSize <= 0) return sbTrack.height - height
            var fracTop = Math.max(0, Math.min(1, (sbTrack.sbSize - view.scrollOffset) / sbTrack.sbSize))
            return fracTop * (sbTrack.height - height)
        }
        MouseArea {
            id: thumbArea
            objectName: "termScrollThumbArea"
            anchors.fill: parent
            hoverEnabled: true
            preventStealing: true
            property real __grab: 0
            onPressed: (m) => { __grab = m.y; sbTrack.dragging = true; sbTrack.pulse() }
            onReleased: sbTrack.dragging = false
            onPositionChanged: (m) => {
                if (!pressed) return
                var maxY = sbTrack.height - sbThumb.height
                if (maxY <= 0) return
                var ny = sbThumb.y + (m.y - __grab)
                var fracTop = Math.max(0, Math.min(ny / maxY, 1))
                view.scrollTo(Math.round((1 - fracTop) * sbTrack.sbSize))
            }
        }
    }
}
)");
    auto *bar = qobject_cast<QQuickItem *>(comp->createWithInitialProperties(
        {{QStringLiteral("view"), QVariant::fromValue<QObject *>(this)}}, qmlContext(this)));
    if (!bar) {
        qWarning("solidterm: failed to compose the scrollbar: %s", qPrintable(comp->errorString()));
        return;
    }
    bar->setParent(this);
    bar->setParentItem(this);
    m_scrollBar = bar;
}

void TerminalView::setFontFamily(const QString &f)
{
    if (m_font.family() == f)
        return;
    m_font.setFamily(f);
    emit fontChanged();
    if (isComponentComplete())
        applyGrid();
    update();
}

void TerminalView::setFontSize(int px)
{
    if (m_font.pixelSize() == px)
        return;
    m_font.setPixelSize(qMax(6, px));
    emit fontChanged();
    if (isComponentComplete())
        applyGrid();
    update();
}

void TerminalView::setBackground(const QColor &c)
{
    if (m_background == c)
        return;
    m_background = c;
    if (m_tmux && m_started)
        tmuxScheduleColors();
    emit colorsChanged();
    update();
}

void TerminalView::setForeground(const QColor &c)
{
    if (m_foreground == c)
        return;
    m_foreground = c;
    if (m_tmux && m_started)
        tmuxScheduleColors();
    emit colorsChanged();
    update();
}

QString TerminalView::currentDirectory() const
{
    if (m_tmux)
        return m_tmux->cwd;
    const qint64 pid = m_pty.pid();
    if (pid <= 0)
        return {};
    const QString cwd = QFileInfo(QStringLiteral("/proc/%1/cwd").arg(pid)).symLinkTarget();
    return QFileInfo(cwd).isDir() ? cwd : QString();
}

void TerminalView::setAnsiPalette(const QStringList &names)
{
    if (m_paletteNames == names)
        return;
    m_paletteNames = names;
    // All-or-nothing: a partial or unparsable list falls back to the xterm defaults entirely.
    m_hasPalette = names.size() == 16;
    for (int i = 0; m_hasPalette && i < 16; ++i) {
        m_palette[i] = QColor::fromString(names.at(i));
        m_hasPalette = m_palette[i].isValid();
    }
    emit colorsChanged();
    update();
}

void TerminalView::setReservedSequences(const QStringList &v)
{
    if (m_reserved == v)
        return;
    m_reserved = v;
    emit reservedSequencesChanged();
}

void TerminalView::setScrollbackLimit(int v)
{
    if (m_scrollbackLimit == v)
        return;
    m_scrollbackLimit = qMax(0, v);
    {
        ScreenLock lock(m_screenMutex);
        m_term.setScrollbackLimit(m_scrollbackLimit); // a lower limit trims at once
    }
    if (m_term.isAttached())
        applyNews(); // clamps the offset
    emit scrollbackLimitChanged();
}

void TerminalView::sendText(const QString &text)
{
    if (!m_vt || m_readOnly)
        return;
    const auto ucs4 = text.toUcs4();
    ScreenLock lock(m_screenMutex);
    for (const char32_t c : ucs4)
        vterm_keyboard_unichar(m_vt, c, VTERM_MOD_NONE);
}
