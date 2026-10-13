#pragma once

#include <QColor>
#include <QPointer>
#include <QQuickWindow>
#include <QQuickItem>
#include <QStringList>
#include <QVariantList>
#include <QVector>

class TerminalPanes;
class TerminalView;
class TmuxSession;
namespace tmux { struct LayoutCell; }

// The session/tab stack (owner's queue: "TABS … manter panes vivos por tab"; + directive: chrome
// in Solid, native only for the terminal). Each tab is a full split tree (TerminalPanes). Switching
// tabs must NOT destroy the others' ptys, so tabs are kept alive HERE — a reactive <For> in the TSX
// would recreate and kill them. This item is HEADLESS: it owns the live panes and shows one by
// index, but paints nothing; the tab BAR is rendered in the TSX from the model this exposes.
//
// Bridge: the TSX drives newTab/closeTab/selectTab and mirrors the model from tabsChanged(titles,
// active) (emitted on any add/close/select/title change). It presents the SAME surface as
// TerminalPanes (style, reservedSequences, split/close/focus, accelerator) proxied to the active tab.
class TerminalTabs : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY styleChanged)
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY styleChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY styleChanged)
    Q_PROPERTY(QColor foreground READ foreground WRITE setForeground NOTIFY styleChanged)
    Q_PROPERTY(QStringList ansiPalette READ ansiPalette WRITE setAnsiPalette NOTIFY styleChanged)
    Q_PROPERTY(int scrollbackLimit READ scrollbackLimit WRITE setScrollbackLimit NOTIFY styleChanged)
    Q_PROPERTY(QColor handleColor READ handleColor WRITE setHandleColor NOTIFY styleChanged)
    Q_PROPERTY(QString backgroundImage READ backgroundImage WRITE setBackgroundImage NOTIFY styleChanged)
    Q_PROPERTY(qreal backgroundOpacity READ backgroundOpacity WRITE setBackgroundOpacity NOTIFY styleChanged)
    Q_PROPERTY(int blur READ blur WRITE setBlur NOTIFY styleChanged)
    Q_PROPERTY(QString scrollbarMode READ scrollbarMode WRITE setScrollbarMode NOTIFY styleChanged)
    Q_PROPERTY(bool animateSplits READ animateSplits WRITE setAnimateSplits NOTIFY styleChanged)
    Q_PROPERTY(int splitGap READ splitGap WRITE setSplitGap NOTIFY styleChanged)
    Q_PROPERTY(bool focusOnHover READ focusOnHover WRITE setFocusOnHover NOTIFY styleChanged)
    Q_PROPERTY(QStringList reservedSequences READ reservedSequences WRITE setReservedSequences NOTIFY reservedChanged)
    Q_PROPERTY(int count READ count NOTIFY tabsChanged)

public:
    explicit TerminalTabs(QQuickItem *parent = nullptr);
    ~TerminalTabs() override;

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
    QString backgroundImage() const { return m_bgImage; }
    void setBackgroundImage(const QString &v);
    qreal backgroundOpacity() const { return m_bgOpacity; }
    void setBackgroundOpacity(qreal v);
    int blur() const { return m_blur; }
    void setBlur(int v);
    QString scrollbarMode() const { return m_scrollbarMode; }
    void setScrollbarMode(const QString &v);
    bool animateSplits() const { return m_animateSplits; }
    void setAnimateSplits(bool v);
    int splitGap() const { return m_splitGap; }
    void setSplitGap(int v);
    bool focusOnHover() const { return m_focusOnHover; }
    void setFocusOnHover(bool v);
    QStringList reservedSequences() const { return m_reserved; }
    void setReservedSequences(const QStringList &v);
    int count() const { return m_tabs.size(); }

    Q_INVOKABLE void newTab();
    Q_INVOKABLE void closeTab(int index);
    Q_INVOKABLE void selectTab(int index);
    // F12 overview: flat list of every pane across every tab ({tab,pane,tabTitle,title,active}).
    Q_INVOKABLE QVariantList overview() const;
    Q_INVOKABLE void focusPane(int tab, int pane); // select the tab + focus that pane
    Q_INVOKABLE void setTabTitle(int index, const QString &title); // custom title (empty = back to OSC)
    Q_INVOKABLE QString tabTitle(int index) const;                 // the effective (custom or OSC) title
    // Proxied to the ACTIVE tab's panes (same names as TerminalPanes, so the TSX is unchanged).
    Q_INVOKABLE void split(int orient);
    Q_INVOKABLE void closeFocused();
    Q_INVOKABLE void focusNext();
    Q_INVOKABLE void focusPrev();
    Q_INVOKABLE void refocus(); // keyboard focus back to the active pane (e.g. closing the search bar)
    Q_INVOKABLE void toggleZoom();
    Q_INVOKABLE int zoomFocused(int delta); // the focused pane's new size (px), 0 if none
    Q_INVOKABLE void copyFocused();
    Q_INVOKABLE void pasteFocused();
    Q_INVOKABLE void pasteTextFocused(const QString &text);
    Q_INVOKABLE void clearFocused();
    Q_INVOKABLE void resetFocused();
    Q_INVOKABLE void setReadOnlyFocused(bool v);
    Q_INVOKABLE void searchFocused(const QString &query);
    Q_INVOKABLE void searchNext();
    Q_INVOKABLE void searchPrev();
    Q_INVOKABLE void clearSearch();
    // The active tab's focused pane's TerminalView::wantsMouseReport. Queried by the solid
    // <ContextMenu> before it opens on a right-click.
    Q_INVOKABLE bool wantsMouseReportFocused(int qtModifiers) const;
    // "Copy link"/"Open link" (right-click menu) acting on the active tab's focused pane's hovered link.
    Q_INVOKABLE void copyHoverLinkFocused();
    Q_INVOKABLE void openHoverLinkFocused();
    // The link the right-click landed on, captured when the menu opened: the popup takes the hover
    // away from the terminal (which clears its hovered link), so the menu acts on this snapshot.
    Q_INVOKABLE void copyLink(const QString &url);
    Q_INVOKABLE void openLink(const QString &url);

    // Pane drag & drop: this item is its WINDOW's drop target. It filters the window's drag events
    // (the compositor delivers them to the window under the cursor — no global coords needed) and
    // routes them: over the panes → the active TerminalPanes' split zones; over the chrome strip
    // above them (header / tab bar) → "new tab here".
    void adoptViewAsTab(TerminalView *v); // a moved live view becomes a new tab (pty rides along)
    static void hideAllTabDropIndicators();
    TerminalPanes *activePanes() const { return active(); }
    int tabCount() const { return m_tabs.size(); }

    // ── tmux mode (docs/solidterm-tmux-control-mode.md 2.1) ──────────────────────────────────────
    // The next TerminalTabs to complete attaches to tmux session `name` instead of opening a local
    // shell (main sets it around the window it builds for `--tmux <name>`). Its tabs are then the
    // session's windows, driven by TmuxSession.
    static void setPendingTmuxSession(const QString &name) { s_pendingTmux = name; }
    bool isTmux() const { return m_isTmux; }
    TmuxSession *tmuxSession() const;
    TerminalPanes *tmuxTab(int window, bool create);  // the tab showing tmux window @window
    void tmuxRemoveTab(int window);
    void tmuxRenameTab(int window, const QString &name);
    void tmuxSelectTab(int window);
    int tmuxTabIndex(int window) const;
    QList<int> tmuxWindows() const;                   // tab order
    QString tmuxTabName(int window) const;
    // The session is gone (exited): the window closes.
    void tmuxSessionEnded();

signals:
    void styleChanged();
    void reservedChanged();
    // The full tab model for the TSX to mirror (titles per tab + the active index). Emitted on any
    // add / close / select / OSC-title change.
    void tabsChanged(const QStringList &titles, int active);
    void allClosed();                          // last tab closed → the app can quit
    void titleChanged(const QString &title);   // active tab's focused-pane title → window
    void accelerator(const QString &sequence); // a reserved chord from the active tab
    void searchChanged(int index, int count);  // active tab's search state → the Solid search bar
    void hoverLinkChanged(const QString &url); // active tab's focused pane's hovered link ("" = none)
    void unsafePasteRequested(const QString &text);
    void zoomRequested(int delta);
    void paneMenuRequested(qreal x, qreal y, bool readOnly);

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void focusInEvent(QFocusEvent *event) override;
    void itemChange(ItemChange change, const ItemChangeData &data) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Tab {
        TerminalPanes *panes = nullptr;
        QString title;         // OSC title from the focused pane
        QString customTitle;   // user-set override; when non-empty it wins
        int tmuxWindow = -1;   // tmux mode: the window id (@N)
        QString tmuxName;      // tmux mode: #{window_name}, shown while there is no OSC title
        QString effective() const
        {
            return !customTitle.isEmpty() ? customTitle : !title.isEmpty() ? title : tmuxName;
        }
    };
    QString windowTitleFor(int index) const; // tmux mode: "<session> — …"

    TerminalPanes *makePanes();
    bool routeDrag(class QDropEvent *event, bool drop); // false = not a SolidTerm pane drag
    void showTabDropIndicator(bool on);
    QPointer<QQuickWindow> m_filteredWindow;
    QQuickItem *m_tabDropOverlay = nullptr;
    static QVector<TerminalTabs *> s_allTabs;
    void applyStyle(TerminalPanes *p);
    void relayout();
    void emitModel();          // push (titles, active) to the TSX
    TerminalPanes *active() const;

    QVector<Tab> m_tabs;
    int m_active = 0;
    QStringList m_lastTitles;  // the model as last emitted (emitModel skips repeats)
    int m_lastActive = -1;
    bool m_modelEmitted = false;
    static inline QString s_pendingTmux;
    bool m_isTmux = false;
    QPointer<TmuxSession> m_tmux;
    bool m_didInitialFocus = false;
    class QQmlComponent *m_panesComponent = nullptr;

    QString m_fontFamily = QStringLiteral("monospace");
    int m_fontSize = 15;
    QColor m_background = QColor("#1e1e1e");
    QColor m_foreground = QColor("#ffffff");
    QStringList m_palette;
    int m_scrollbackLimit = 8000;
    QColor m_handleColor = QColor("#151515");
    QString m_bgImage;
    qreal m_bgOpacity = 1.0;
    int m_blur = 0;                         // 0-100 blur strength on the background image
    QString m_scrollbarMode = QStringLiteral("auto");
    bool m_animateSplits = true;
    int m_splitGap = 0;
    bool m_focusOnHover = false;
    QStringList m_reserved;
};
