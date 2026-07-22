#pragma once

#include "ptysession.h"

#include <QColor>
#include <QFont>
#include <QQuickPaintedItem>
#include <QStringList>
#include <QVector>

#include <vterm.h>

// The terminal pane: a QQuickItem hosting one shell session. libvterm does the VT/xterm
// interpretation (escape parsing, screen model, damage) — the same core neovim embeds; this
// item feeds it pty bytes, renders its screen (run-merged cells; scene-graph textured, so GL/RHI
// accelerated like every Quick item), forwards keys, and keeps a scrollback ring.
//
// Registered by the app binary as `SolidTerm 1.0` and consumed from TSX via
// `import { TerminalView } from "qml:SolidTerm"` — the app's OWN C++ entering the solid UI
// through the standard qml-module door (Direção B).
class TerminalView : public QQuickPaintedItem {
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

    // C++-created panes (TerminalPanes uses `new`, so componentComplete never fires) call this
    // once sized to boot the pty+vterm; idempotent (m_started guard).
    Q_INVOKABLE void ensureStarted();

    Q_INVOKABLE void sendText(const QString &text);       // paste path
    Q_INVOKABLE void takeFocus() { forceActiveFocus(Qt::MouseFocusReason); }
    bool hasSelection() const { return m_selValid; }
    Q_INVOKABLE void copySelection();
    Q_INVOKABLE void pasteClipboard();
    Q_INVOKABLE void clearScrollback();

    void paint(QPainter *p) override;

signals:
    void fontChanged();
    void colorsChanged();
    void titleChanged();
    void scrollbackLimitChanged();
    void bellRang();
    void sessionFinished();
    void selectionChanged();
    void reservedSequencesChanged();
    void accelerator(const QString &sequence); // a reserved chord was pressed (consumed)

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

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
    void paintCells(QPainter *p);
    QColor toQColor(VTermColor c, bool isFg) const;
    void keyToVTerm(QKeyEvent *event);

    VTerm *m_vt = nullptr;
    VTermScreen *m_screen = nullptr;
    PtySession m_pty;
    QFont m_font;
    qreal m_cellW = 8, m_cellH = 16, m_cellAscent = 12;
    int m_rows = 24, m_cols = 80;
    QColor m_background = QColor("#161a21");
    QColor m_foreground = QColor("#d4dae3");
    QString m_title;
    VTermPos m_cursor = { 0, 0 };
    bool m_cursorVisible = true;
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
};
