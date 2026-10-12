#pragma once

class QQuickWindow;
class QMimeData;
class TerminalTabs;

#include <QPixmap>
#include <QPointer>
#include <QQuickItem>
#include <QStringList>
#include <QUrl>
#include <QVector>

class TerminalView;
class PaneHeader;

// The split container (owner: "split screen … parecido com tilix"). Splitting a terminal is
// terminal-domain layout that For→SplitView can't express through the transpiler subset, so it's
// native C++ here — a BINARY SPLIT TREE (like tilix): every internal node is a horizontal or
// vertical split of its children, leaves are terminals. A leaf split H then V then H nests
// arbitrarily, so Alt+D (split right) and Alt+Shift+D (split down) always do what they say,
// regardless of what came before — unlike the old flat single-orientation list, where the first
// split locked the orientation for the whole container.
//
// Logical tree only: every leaf cell and every divider is still a direct child of this item,
// positioned by a recursive layout walk — so the scene graph stays flat and animations/focus are
// unchanged.
class TerminalPanes : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY panesChanged)
    // Forwarded to every pane so preferences apply live across splits.
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY styleChanged)
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY styleChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY styleChanged)
    Q_PROPERTY(QColor foreground READ foreground WRITE setForeground NOTIFY styleChanged)
    Q_PROPERTY(QStringList ansiPalette READ ansiPalette WRITE setAnsiPalette NOTIFY styleChanged)
    Q_PROPERTY(int scrollbackLimit READ scrollbackLimit WRITE setScrollbackLimit NOTIFY styleChanged)
    Q_PROPERTY(QColor handleColor READ handleColor WRITE setHandleColor NOTIFY styleChanged)
    Q_PROPERTY(QStringList reservedSequences READ reservedSequences WRITE setReservedSequences NOTIFY reservedChanged)
    // Decorations forwarded to every pane (per-window eye candy).
    Q_PROPERTY(QString backgroundImage READ backgroundImage WRITE setBackgroundImage NOTIFY styleChanged)
    Q_PROPERTY(qreal backgroundOpacity READ backgroundOpacity WRITE setBackgroundOpacity NOTIFY styleChanged)
    Q_PROPERTY(int blur READ blur WRITE setBlur NOTIFY styleChanged)

public:
    // Where a dragged pane lands on a target pane: centre (swap) or a side (re-split).
    enum DropZone { ZoneNone, ZoneCenter, ZoneLeft, ZoneRight, ZoneTop, ZoneBottom, ZoneNewTab };

    explicit TerminalPanes(QQuickItem *parent = nullptr);
    ~TerminalPanes() override;

    int count() const;

    QString fontFamily() const { return m_fontFamily; }
    void setFontFamily(const QString &v);
    int fontSize() const { return m_fontSize; }
    void setFontSize(int v);
    QColor background() const { return m_background; }
    void setBackground(const QColor &v);
    QColor foreground() const { return m_foreground; }
    void setForeground(const QColor &v);
    QStringList ansiPalette() const { return m_palette; }
    void setAnsiPalette(const QStringList &v);
    int scrollbackLimit() const { return m_scrollbackLimit; }
    void setScrollbackLimit(int v);
    QColor handleColor() const { return m_handleColor; }
    void setHandleColor(const QColor &v);
    QStringList reservedSequences() const { return m_reserved; }
    void setReservedSequences(const QStringList &v);
    QString backgroundImage() const { return m_bgImage; }
    void setBackgroundImage(const QString &v);
    qreal backgroundOpacity() const { return m_bgOpacity; }
    void setBackgroundOpacity(qreal v);
    int blur() const { return m_blur; }
    void setBlur(int v);
    bool animateSplits() const { return m_animateSplits; }
    void setAnimateSplits(bool v) { m_animateSplits = v; }
    // Gap between split panes, px. 0 = the classic 6 px handle-coloured divider; above 0 the divider
    // is that wide and transparent (the window background shows between panes) — still draggable.
    int splitGap() const { return m_splitGap; }
    void setSplitGap(int v);
    // Focus follows the mouse between panes — only while THIS window is already the active one.
    bool focusOnHover() const { return m_focusOnHover; }
    void setFocusOnHover(bool v) { m_focusOnHover = v; }

    // Split the FOCUSED pane along `orient` (Qt::Horizontal = side by side / "split right",
    // Qt::Vertical = stacked / "split down"). Nests independently of prior splits.
    Q_INVOKABLE void split(int orient);
    Q_INVOKABLE void closeFocused();
    Q_INVOKABLE void focusNext();
    Q_INVOKABLE void focusPrev();
    Q_INVOKABLE void refocus(); // re-take keyboard focus on the current pane (tab switch)
    Q_INVOKABLE void toggleZoom(); // maximise the focused pane to fill the session; again restores
    Q_INVOKABLE int zoomFocused(int delta); // per-pane FONT zoom (delta 0 = reset to the shared size)
    QStringList paneTitles() const;         // leaf titles in order (for the F12 overview)
    int focusedPaneIndex() const;           // index of the focused leaf
    Q_INVOKABLE void focusLeafByIndex(int i);
    // Debug/test hook: drive the real drag path — drag leaf `srcIdx` onto `zone` (DropZone values:
    // 1=centre 2=left 3=right 4=top 5=bottom) of leaf `dstIdx`. Lets the grab harness exercise
    // rearrange without a synthetic gesture.
    Q_INVOKABLE void debugDragLeaf(int srcIdx, int dstIdx, int zone);
    Q_INVOKABLE void debugDetachFocused(); // test hook: detach the focused pane to a new window
    // Test hook: drag the newest window's focused pane onto the FIRST other window — `zone` 1-5 of
    // that window's first pane, or ZoneNewTab = its tab strip — through the real drop path
    // (synthetic QDragEnter/Move/Drop events to the window + the same finish step a QDrag runs).
    // `hoverShot`: grab the target window while the drop indicator shows (empty = no grab).
    static void debugReattachLastToFirst(int zone = ZoneRight, const QString &hoverShot = QString());
    // Test hook: deliver a synthetic drag of `src` to `window` at scene `pos` and finish it.
    // `drop` false = hover only (the indicator shows; the session stays open until finishDrag).
    static void debugSyntheticDrag(TerminalView *src, QQuickWindow *window, const QPointF &pos, bool drop);

    // ── Pane drag & drop (cross-window, Wayland-safe) ──────────────────────────────────────────
    // A header drag is a real QDrag carrying paneMimeType() ("<pid>:<serial>"). Every SolidTerm
    // window is a drop target (TerminalTabs filters its window's drag events), so the compositor —
    // not global coordinates, which Wayland does not give — says which window the cursor is over.
    // The drop only RECORDS the target; the source applies it once QDrag::exec has returned (after
    // the header's stack unwound). An unaccepted drag (Esc, outside every SolidTerm window)
    // detaches the pane to a new window, as before.
    static QString paneMimeType();
    static QMimeData *makeDragMime(TerminalView *v); // opens a drag session for v
    static TerminalView *dragSource(const QMimeData *mime); // the live source if `mime` is OUR session
    static bool dragInProgress() { return !s_dragSource.isNull(); }
    static void finishDrag(TerminalView *v);         // apply the recorded drop, else detach
    static void recordNewTabDrop(class TerminalTabs *tabs);
    static void recordNoopDrop();
    static TerminalPanes *ownerOf(TerminalView *v);  // the container holding v (any window)
    static void hideAllDropIndicators();
    // Called with the drag-icon window once a drag starts (main attaches the compositor blur).
    static void setDragIconHook(void (*hook)(QWindow *)) { s_dragIconHook = hook; }
    // The next container to be created adopts `v` as its first pane (detach / drop-as-new-tab).
    static void adoptOnNextCreate(TerminalView *v) { s_pendingAdopt = v; }
    // Drop-target side, in this container's local coords. dragHover shows the indicator and
    // returns the zone (ZoneNone = no-op here); dragDrop records the drop for finishDrag.
    int dragHover(TerminalView *src, const QPointF &local);
    void dragDrop(TerminalView *src, const QPointF &local);
    void dragClear();
    // The drag preview: a translucent, rounded snapshot of v's cell (keeps the window's alpha).
    QPixmap dragPreview(TerminalView *v, const QPointF &grabInCell, QPoint *hotSpot) const;
    TerminalView *focusedView() const;
    TerminalView *viewAt(int leafIndex) const;

    // The URL of the top-level solidterm Window component (App.generated.qml), set by main() — used to
    // spawn a full new window when a pane is detached.
    static void setWindowUrl(const QUrl &url);
    // Called with every window a pane detach creates (the app wires it like its first window).
    static void setWindowCreatedHook(void (*hook)(QQuickWindow *)) { s_windowCreated = hook; }
    static bool anyLive() { return !s_all.isEmpty(); } // any terminal pane alive in any window?
    // Paste/copy proxied to the focused pane (menu actions).
    Q_INVOKABLE void copyFocused();
    Q_INVOKABLE void pasteFocused();
    Q_INVOKABLE void pasteTextFocused(const QString &text); // confirmed paste → focused pane
    Q_INVOKABLE void clearFocused();
    Q_INVOKABLE void resetFocused();
    Q_INVOKABLE void setReadOnlyFocused(bool v);
    // Search proxied to the focused pane.
    Q_INVOKABLE void searchFocused(const QString &query);
    Q_INVOKABLE void searchNext();
    Q_INVOKABLE void searchPrev();
    Q_INVOKABLE void clearSearch();
    // The focused pane's TerminalView::wantsMouseReport — see there for what it means. Queried by
    // the solid <ContextMenu> before it opens on a right-click.
    Q_INVOKABLE bool wantsMouseReportFocused(int qtModifiers) const;

signals:
    void panesChanged();
    void searchChanged(int index, int count);
    void unsafePasteRequested(const QString &text);
    void zoomRequested(int delta);
    void paneMenuRequested(qreal x, qreal y, bool readOnly); // header title clicked → open dropdown
    void styleChanged();
    void allClosed();           // last pane's shell exited → the app can quit/close the tab
    void titleChanged(const QString &title); // focused pane's OSC title
    void reservedChanged();
    void accelerator(const QString &sequence); // a reserved chord pressed in any pane

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void focusInEvent(QFocusEvent *event) override;

private:
    // A node in the split tree. Leaf: children empty, holds cell/view/header. Split: 2+ children
    // laid out along `orientation` by `fractions`, with `dividers` between them.
    struct Node {
        Node *parent = nullptr;
        int orientation = 1;            // Qt::Horizontal(1) / Qt::Vertical(2) for splits
        QVector<Node *> children;
        QVector<qreal> fractions;       // per-child share (sums to 1)
        qreal lastAvail = 1;            // main-axis px length from last layout (for divider drag px→fraction)
        QVector<class DividerItem *> dividers; // children.size()-1
        // Leaf payload:
        QQuickItem *cell = nullptr;
        TerminalView *view = nullptr;
        PaneHeader *header = nullptr;
        bool isLeaf() const { return children.isEmpty(); }
    };

    // `startCwd`: directory a FRESH view's shell starts in (a split passes the focused pane's);
    // empty = the window's spawn context. Ignored when adopting a moved view.
    Node *makeLeaf(TerminalView *adopt = nullptr, const QString &startCwd = QString()); // compose a cell; adopt a moved view or make one
    void wirePane(TerminalView *v, PaneHeader *header);
    void applyStyle(TerminalView *v);

    void layoutNode(Node *node, qreal x, qreal y, qreal w, qreal h);
    void rebuildDividers(Node *splitNode); // (re)create the divider items for a split node
    void collectLeaves(Node *node, QVector<Node *> &out) const;
    void insertLeafBeside(Node *anchor, Node *fresh, int orient, bool before); // splice a leaf next to another
    void unlinkLeaf(Node *leaf);        // detach a leaf from its parent + collapse a single-child split (node kept)
    void removeLeaf(Node *leaf);
    void deleteSubtree(Node *node);     // frees dividers + cells recursively
    void setFocused(Node *leaf);
    Node *leafOfView(TerminalView *v) const;
    void refreshHeaderFocus();
    void relayout();

    // Header-drag pane rearrange (drop a pane onto a side of another to re-split; onto the centre to
    // swap — within one container; across containers the centre folds into the nearest side).
    void beginPaneDrag(TerminalView *v, const QPointF &pressInCell); // runs the QDrag (nested loop)
    Node *leafAtLocal(const QPointF &p) const;      // leaf whose cell contains a TerminalPanes-local point
    int zoneAt(const QPointF &inCell, qreal w, qreal h, bool allowCenter = true) const;
    QRectF zoneRect(Node *leaf, int zone) const;    // highlight rect (TerminalPanes coords) for a zone
    void moveLeaf(TerminalView *v, Node *target, int side); // re-split: move the live pane beside target
    void swapLeaves(TerminalView *v, Node *target);         // centre drop: exchange two panes' positions
    // Cross-window drag: detach the live view from this tree (keeping the pty), then either splice it
    // into another window's tree (reattach) or spawn a fresh window around it (detach).
    TerminalView *detachViewKeepAlive(TerminalView *v);     // remove from this tree, return the live view
    void insertExternalView(TerminalView *v, Node *target, int zone); // adopt a view from another window
    void detachToNewWindow(TerminalView *v);

    Node *m_root = nullptr;
    Node *m_focused = nullptr;          // focused LEAF
    bool m_zoomed = false;              // one pane maximised to fill the session
    Node *m_zoomLeaf = nullptr;
    bool m_didInitialFocus = false;
    class QQmlComponent *m_cellComponent = nullptr;
    class QQmlComponent *m_viewComponent = nullptr;

    // All live pane containers across every window (for cross-window drag hit-testing).
    static QVector<TerminalPanes *> s_all;
    static QUrl s_windowUrl;             // App.generated.qml Window component
    static inline void (*s_windowCreated)(QQuickWindow *) = nullptr;
    static TerminalView *s_pendingAdopt; // a detached view the next new window should adopt as pane 1

    // Drag session (process-wide: one pointer, one drag at a time).
    static QPointer<TerminalView> s_dragSource; // non-null while a pane drag is live
    static quint64 s_dragSerial;
    struct PendingDrop {
        quint64 serial = 0;               // the session it belongs to (0 = nothing recorded)
        bool noop = false;                // dropped where nothing changes (e.g. its own pane)
        QPointer<TerminalPanes> panes;    // split drop: target container…
        QPointer<TerminalView> anchor;    // …and the pane it lands beside (views persist, nodes may not)
        int zone = ZoneNone;
        QPointer<class TerminalTabs> tabs; // tab-strip drop: a new tab in that window
    };
    static PendingDrop s_pending;
    static inline void (*s_dragIconHook)(QWindow *) = nullptr;
    QQuickItem *ensureDropOverlay();
    QQuickItem *m_dropOverlay = nullptr;  // translucent highlight over the hovered drop zone

    QString m_fontFamily = QStringLiteral("monospace");
    int m_fontSize = 15;
    QColor m_background = QColor("#1e1e1e");
    QColor m_foreground = QColor("#ffffff");
    QStringList m_palette;
    int m_scrollbackLimit = 8000;
    // Pane title strip: the handle colour lifted a touch, as translucent as the terminal behind it
    // (an opaque strip over translucent panes read as a heavy black bar).
    QColor headerColor() const;
    void recolorHandles(); // dividers + headers across the tree
    QColor m_handleColor = QColor("#151515");
    QStringList m_reserved;
    QString m_bgImage;
    qreal m_bgOpacity = 1.0;
    int m_blur = 0;                         // 0-100 blur strength on the background image
    bool m_animateSplits = true;
    int m_splitGap = 0;
    bool m_focusOnHover = false;
    qreal dividerExtent() const;  // the gap when set, else the 6 px handle
    QColor dividerColor() const;  // transparent when there is a gap, else the handle colour
};
