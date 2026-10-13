#pragma once

#include <QHash>
#include <QSet>
#include <QVariantList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace tmux { class Client; struct Event; struct LayoutCell; }
class TerminalTabs;
class TerminalView;
class TermConfig;
class QQuickWindow;
class QProcess;
class QSocketNotifier;
namespace tmux { class CommandLine; }

// One SolidTerm window attached to one tmux session through one control client
// (docs/solidterm-tmux-control-mode.md 2.1, 5.1). GUI thread.
//
// The model is SERVER-DRIVEN (2.5): tabs and split trees change only when tmux says so
// (%window-add/close, %layout-change, list replies); nothing is changed optimistically. Input
// (phase 2) goes per pane from TerminalView: keys as send-keys names/literals, mouse, focus and
// paste as send-keys -H bytes. Structural actions (split, close, new tab) are phase 3.
//
// Lifetime: owned by the application, not by the window — closing the window detaches (the
// session lives on in tmux) and this object goes once the tmux client has exited.
class TmuxSession : public QObject {
    Q_OBJECT

public:
    // attachOnly: `attach-session -t <name>` (fails when there is no such session) instead of
    // `new-session -A` (attach, or create it).
    TmuxSession(const QString &name, TerminalTabs *tabs, bool attachOnly = false);

    // ── lifecycle (phase 4) ─────────────────────────────────────────────────────────────────────
    // Preferences (6.5): tmuxBinary, tmuxPauseAfter (s), tmuxHoldCap (MiB), tmuxHistory (lines; 0 =
    // the scrollback limit), tmuxCloseAction ("detach" | "kill"), tmuxReattach (0/1). Read at use;
    // pause-after and the cap also apply live to every session.
    static void setConfig(TermConfig *config);
    static TermConfig *config();
    static QString binary();                      // $SOLIDTERM_TMUX_BIN > tmuxBinary > "tmux"
    static constexpr int kMinMajor = 3, kMinMinor = 5; // 7: pause-after/-A (3.2), %config-error (3.4), -r (3.5)
    // "tmux 3.7c" / "tmux next-3.6" / "tmux master" → ok, or why not.
    static bool versionOk(const QString &versionLine, QString *why);
    // The live session showing `name` (5.2: a second request raises its window instead).
    static TmuxSession *find(const QString &name);
    QQuickWindow *window() const;
    ~TmuxSession() override;

    std::shared_ptr<tmux::Client> client() const { return m_client; }
    qint64 clientPid() const;               // tests: the `tmux -C` process
    QString name() const { return m_name; }
    QString paneCwd(int pane) const { return m_paneCwd.value(pane); }
    bool attached() const { return m_attached; }
    QString lastError() const { return m_error; }

    // The panes area changed (px) or the font did: the client size in cells, sent debounced.
    void setArea(qreal width, qreal height, qreal cellW, qreal cellH);
    // The window is closing: detach (tmux keeps the session); this object goes when the client ends.
    void detach();

    // A view is about to send input to `pane` (keys, mouse, paste). tmux's active pane matters for
    // commands without -t and for other clients (2.4): when it is not this one, select-pane goes out
    // first — lazily, so focus-follows-mouse does not send one per hover. Plain fn-ptr shape
    // (TerminalView::setTmuxInputHook).
    static void beforeInput(void *session, int pane);

    // ── structural actions (2.5): each sends a tmux command and changes nothing locally; the
    // notifications it causes update the model (one path for SolidTerm, tmux key bindings and other
    // clients alike) ─────────────────────────────────────────────────────────────────────────────
    void split(int pane, int orient);                       // split-window -h|-v -c '#{pane_current_path}'
    void newWindow(int afterWindow);                        // new-window -a -t @W
    void selectWindow(int window);                          // select-window (echo suppressed)
    void zoom(int pane);                                    // resize-pane -Z
    void resizePane(int pane, bool horizontal, int cells);  // resize-pane -x|-y
    void swapPane(int a, int b);                            // swap-pane
    void movePane(int a, int b, bool horizontal, bool before); // move-pane -h|-v [-b]
    void breakPane(int pane);                               // break-pane → a new tab
    void killPane(int pane);
    void killWindow(int window);
    void leaveMode(int pane);                               // send-keys -X cancel
    void renameWindow(int window, const QString &name);
    // Owner decision 2: closing HIDES. A pane alone in its window hides the window; otherwise it is
    // broken out (break-pane -d) into a window that is hidden as it arrives. Hidden windows stay in
    // tmux (programs keep running); they are listed by hidden() and come back with restoreHidden():
    // a broken-out pane rejoins its window (join-pane) when that still exists, else its tab shows
    // again. Not persisted: a reattach shows everything again. Hiding the last visible tab detaches.
    void hidePane(int pane);
    void killSession();                                     // kill-session (ends its programs)
    void hideWindow(int window);
    QVariantList hidden() const;                            // [{window, title}]
    void restoreHidden(int window);

    // Views of panes that left a window's layout wait here: another window naming the pane adopts
    // the live view (2.2); the next list-windows reply (tmux's state after those notifications)
    // drops the rest.
    void parkView(int pane, TerminalView *view);
    TerminalView *takeParked(int pane);
    // The live view of `pane` for `into`: parked, or still in ANOTHER window's tree (the destination's
    // %layout-change can come before the source's): taken out of that tree. Null: a new pane.
    TerminalView *takeView(int pane, class TerminalPanes *into);

    // The window title for a focused-pane title (2.4): "<session> — <title>".
    QString windowTitle(const QString &paneTitle) const;

    // Tests: counters of the client (seeds/reseeds done) and the scenario's step handshake.
    int seedsCompleted() const;
    int reseedsCompleted() const;
    void sendTestStep(const QString &step); // display-message -p '<step>' (a fake tmux reacts to it)
    void sendRaw(const QString &command);   // one already-quoted command line (tests only)
    void setTee(bool on);
    QByteArray tee(int pane) const;

signals:
    void exited(const QString &reason);     // the control client ended (detached, killed, server gone)
    // A problem to show over the window (version, missing tmux, the server or client gone).
    // `reattach`: offering "Reattach" makes sense. Empty text: nothing to show.
    void banner(const QString &text, bool reattach);
    void finished();                        // reaped: the object is about to go
    void stepReplied(const QString &step);  // tests: a sendTestStep answered
    void hiddenChanged();                   // hidden() changed

private:
    void onEvents();
    void handle(tmux::Event &e);
    void startAttach();
    void preflightDone();
    void spawn();
    void fail(const QString &why, bool reattach);
    void lost(const QString &reason);       // the client ended under us: banner + read-only panes
    void requestLists(bool initial);
    void applyWindowList(const QStringList &lines, bool initial);
    void applyLayout(int window, const QString &layout, const QString &visible, const QString &flags, bool focusActive);
    void finish();
    void sendSize();
    void send(const tmux::CommandLine &line);
    void flushParked();
    void applyModes();
    bool hideIfPending(int window, const tmux::LayoutCell &root);
    void maybeDetachEmpty();

    QString m_name;
    QPointer<TerminalTabs> m_tabs;
    std::shared_ptr<tmux::Client> m_client;
    QSocketNotifier *m_notifier = nullptr;
    int m_sessionId = -1;
    bool m_attachOnly = false;
    bool m_createdByUs = false;           // has-session said no: our new-session creates it (decision 3)
    QProcess *m_probe = nullptr;          // tmux -V / has-session, before the control client
    int m_probeStep = 0;
    QString m_lastStderr;
    bool m_attached = false;
    bool m_detaching = false;
    bool m_finished = false;
    bool m_built = false;                  // the initial lists were applied
    bool m_floatingNoticed = false;
    QStringList m_pendingWindows;          // the initial list-windows, waiting for list-panes
    QHash<int, QString> m_paneCwd;
    QHash<int, int> m_activePane;          // window → tmux's active pane (as we last know or set it)
    QHash<int, int> m_paneWindow;          // pane → its window (from the layouts)
    QHash<int, QString> m_paneMode;        // pane → its tmux mode ("" none): the header badge (6.6)
    QHash<int, QPointer<TerminalView>> m_parked;
    bool m_flushPending = false;
    QHash<int, int> m_pendingHide;         // broken-out pane → its origin window, until its window shows up
    QHash<int, int> m_hiddenOrigin;        // hidden window → origin window of its broken-out pane (-1: a hidden tab)
    int m_selectedWindow = -1;             // what we last made current (select-window echo)
    int m_serverWindow = -1;               // the session's current window as tmux last said
    int m_cols = 0, m_rows = 0, m_sentCols = -1, m_sentRows = -1;
    QTimer m_sizeTimer;
    QTimer m_detachTimer;
    QString m_error;
};
