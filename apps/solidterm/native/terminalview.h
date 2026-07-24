#pragma once

#include "glyphcache.h"
#include "ptysession.h"

#include <QColor>
#include <QFont>
#include <QQuickItem>
#include <QStringList>
#include <QVector>

#include <vterm.h>

class QSGTexture;
class QSGImageNode;
class QSGGeometryNode;

// The terminal pane: a QQuickItem hosting one shell session. libvterm does the VT/xterm
// interpretation (escape parsing, screen model, damage) — the same core neovim embeds; this
// item feeds it pty bytes and renders its screen straight on the scene graph (GPU): each glyph is
// rasterised ONCE into a coverage atlas (GlyphCache) and the whole grid is drawn as textured quads
// through a custom RHI material — no per-frame QPainter, "as fast as alacritty" (owner). Keeps a
// scrollback ring, forwards keys, tracks selection.
//
// Registered by the app binary as `SolidTerm 1.0` and consumed from TSX via
// `import { TerminalView } from "qml:SolidTerm"` — the app's OWN C++ entering the solid UI
// through the standard qml-module door (Direção B).
class TerminalView : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontChanged)
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY fontChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY colorsChanged)
    Q_PROPERTY(QColor foreground READ foreground WRITE setForeground NOTIFY colorsChanged)
    // OSC 0/2 window title from the running program (prompt integration etc.).
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(int scrollbackLimit READ scrollbackLimit WRITE setScrollbackLimit NOTIFY scrollbackLimitChanged)
    // Accelerator sequences the terminal must CONSUME (not send to the pty) — terminal apps own
    // their keybindings, and Qt's Shortcut map ambiguates overlapping combos (Alt+D vs Alt+Shift+D)
    // and leaks the key. Set from the solid keybinding config; a match emits accelerator(seq).
    Q_PROPERTY(QStringList reservedSequences READ reservedSequences WRITE setReservedSequences NOTIFY reservedSequencesChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool readOnly READ readOnly WRITE setReadOnly NOTIFY readOnlyChanged) // ignore keyboard/paste
    // Eye candy (owner): a background image behind the text (cover-fit), a background opacity so the
    // desktop/image shows through the terminal's own colour, and an engraved "emboss" on the glyphs.
    Q_PROPERTY(QString backgroundImage READ backgroundImage WRITE setBackgroundImage NOTIFY decorChanged)
    Q_PROPERTY(qreal backgroundOpacity READ backgroundOpacity WRITE setBackgroundOpacity NOTIFY decorChanged)
    Q_PROPERTY(bool emboss READ emboss WRITE setEmboss NOTIFY decorChanged)

public:
    explicit TerminalView(QQuickItem *parent = nullptr);
    ~TerminalView() override;

    QString fontFamily() const { return m_font.family(); }
    void setFontFamily(const QString &f);
    int fontSize() const { return m_font.pixelSize(); }
    void setFontSize(int px);
    QColor background() const { return m_background; }
    void setBackground(const QColor &c);
    QColor foreground() const { return m_foreground; }
    void setForeground(const QColor &c);
    QString title() const { return m_title; }
    int scrollbackLimit() const { return m_scrollbackLimit; }
    void setScrollbackLimit(int v);
    QStringList reservedSequences() const { return m_reserved; }
    void setReservedSequences(const QStringList &v);
    QString backgroundImage() const { return m_bgImagePath; }
    void setBackgroundImage(const QString &path);
    qreal backgroundOpacity() const { return m_bgOpacity; }
    void setBackgroundOpacity(qreal v);
    bool emboss() const { return m_emboss; }
    void setEmboss(bool v);

    // C++-created panes (TerminalPanes uses `new`, so componentComplete never fires) call this
    // once sized to boot the pty+vterm; idempotent (m_started guard).
    Q_INVOKABLE void ensureStarted();

    Q_INVOKABLE void sendText(const QString &text);       // paste path
    Q_INVOKABLE void takeFocus() { forceActiveFocus(Qt::MouseFocusReason); }
    bool hasSelection() const { return m_selValid; }
    bool readOnly() const { return m_readOnly; }
    void setReadOnly(bool v);
    Q_INVOKABLE void copySelection();
    Q_INVOKABLE void pasteClipboard();      // paste the clipboard (guarded: multiline → confirm)
    Q_INVOKABLE void pasteText(const QString &text); // send text verbatim (post-confirm path)
    Q_INVOKABLE void clearScrollback();
    Q_INVOKABLE void resetTerminal();       // soft reset (attrs/cursor/modes back to defaults)
    Q_INVOKABLE void setDimmed(bool v);     // draw a subtle dark overlay (unfocused split pane)
    // Search the scrollback + screen (case-insensitive). Highlights matches, jumps to one, and
    // emits searchChanged(index, count) for the Solid search bar. next/prev cycle; clear removes.
    Q_INVOKABLE void search(const QString &query);
    Q_INVOKABLE void searchNext();
    Q_INVOKABLE void searchPrev();
    Q_INVOKABLE void clearSearch();

    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;

signals:
    void fontChanged();
    void colorsChanged();
    void decorChanged();
    void titleChanged();
    void scrollbackLimitChanged();
    void bellRang();
    void sessionFinished();
    void selectionChanged();
    void readOnlyChanged();
    void reservedSequencesChanged();
    void accelerator(const QString &sequence); // a reserved chord was pressed (consumed)
    void searchChanged(int index, int count);  // current match (1-based; 0 = none) + total
    void unsafePasteRequested(const QString &text); // multiline paste → confirm in the Solid dialog
    void zoomRequested(int delta);          // Ctrl+wheel → font zoom (+1 in / −1 out)
    void focusRequested();                  // user clicked in the body → make this the focused pane

protected:
    void componentComplete() override;
    void itemChange(ItemChange change, const ItemChangeData &data) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;

public:
    // libvterm callbacks (static thunks → the view; public for the file-static callback table).
    static int cbDamage(VTermRect rect, void *user);
    static int cbMoveCursor(VTermPos pos, VTermPos oldPos, int visible, void *user);
    static int cbSetTermProp(VTermProp prop, VTermValue *val, void *user);
    static int cbBell(void *user);
    static int cbSbPushLine(int cols, const VTermScreenCell *cells, void *user);
    static int cbSbPopLine(int cols, VTermScreenCell *cells, void *user);

private:
    struct SbLine {
        QVector<VTermScreenCell> cells;
    };

    void ensureSession();
    void applyGrid();                       // width/height/font → rows/cols → vterm + pty
    QColor toQColor(VTermColor c, bool isFg) const;
    void keyToVTerm(QKeyEvent *event);

    // Auto-detected hyperlink (URL) under the mouse: absolute row + col span + the URL. Hovering
    // underlines it; Ctrl+click opens it. Detected by regex over the row text.
    struct LinkSpan {
        int absRow = 0;
        int c0 = 0, c1 = -1;   // inclusive column range
        QString url;
        bool valid = false;
    };
    LinkSpan linkAt(const QPointF &pos) const;
    LinkSpan m_hoverLink;

    // Scrollback search.
    struct Match { int absRow = 0; int c0 = 0; int len = 0; };
    QString rowText(int absRow) const;      // col-indexed text of an absolute row
    void scrollToMatch(int i);
    QString m_searchQuery;
    QVector<Match> m_matches;
    int m_searchIndex = -1;                 // index into m_matches, -1 = none

    VTerm *m_vt = nullptr;
    VTermScreen *m_screen = nullptr;
    PtySession m_pty;
    QFont m_font;
    GlyphCache m_glyphs;
    qreal m_cellW = 8, m_cellH = 16, m_cellAscent = 12;
    int m_rows = 24, m_cols = 80;
    QColor m_background = QColor("#161a21");
    QColor m_foreground = QColor("#d4dae3");
    QString m_bgImagePath;
    QImage m_bgImage;                       // loaded background image (empty = none)
    bool m_bgImageDirty = false;            // reupload the image texture next frame
    qreal m_bgOpacity = 1.0;                // terminal background alpha (1 = solid, 0 = fully see-through)
    bool m_emboss = false;                  // engraved glyphs
    bool m_dimmed = false;                  // dark overlay for an unfocused split pane
    bool m_bellActive = false;              // visual bell flash in progress (short-timer cleared)
    bool m_readOnly = false;                // block keyboard + paste to the pty
    // Scene-graph nodes (owned by the returned root), kept so the optional image layer can slot in
    // below the others without index juggling. Nulled when the node tree is dropped (0-size).
    QSGImageNode *m_imageNode = nullptr;
    QSGGeometryNode *m_bgNode = nullptr;
    QSGGeometryNode *m_glyphNode = nullptr;
    QSGGeometryNode *m_cursorNode = nullptr;
    QString m_title;
    VTermPos m_cursor = { 0, 0 };
    bool m_cursorVisible = true;
    class QTimer *m_blinkTimer = nullptr;   // blinks the focused cursor
    bool m_blinkOn = true;
    void resetBlink();                      // solid cursor on activity, restart the blink phase
    QVector<SbLine> m_scrollback;           // ring, newest at the back
    int m_scrollbackLimit = 8000;
    QStringList m_reserved;
    int m_scrollOffset = 0;                 // lines scrolled back from live (0 = live)
    bool m_started = false;

    // Selection in ABSOLUTE line space (scrollback index; live rows continue past the ring), so
    // it survives scrolling. Anchor = press point; end tracks the drag.
    struct CellPos {
        int absRow = 0;
        int col = 0;
    };
    CellPos cellAt(const QPointF &p) const;
    void selectedRange(CellPos &from, CellPos &to) const;
    QString selectedText() const;
    CellPos m_selAnchor, m_selEnd;
    bool m_selValid = false;
    bool m_selecting = false;
    quint64 m_lastDblTime = 0;               // for triple-click (line select) detection
    void selectWordAt(const CellPos &c);
    void selectLineAt(const CellPos &c);
};
