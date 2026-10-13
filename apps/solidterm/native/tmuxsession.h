#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace tmux { class Client; struct Event; }
class TerminalTabs;
class QSocketNotifier;

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
    TmuxSession(const QString &name, TerminalTabs *tabs);
    ~TmuxSession() override;

    std::shared_ptr<tmux::Client> client() const { return m_client; }
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
    void finished();                        // reaped: the object is about to go
    void stepReplied(const QString &step);  // tests: a sendTestStep answered

private:
    void onEvents();
    void handle(tmux::Event &e);
    void startAttach();
    void requestLists(bool initial);
    void applyWindowList(const QStringList &lines, bool initial);
    void applyLayout(int window, const QString &layout, const QString &visible, const QString &flags, bool focusActive);
    void finish();
    void sendSize();

    QString m_name;
    QPointer<TerminalTabs> m_tabs;
    std::shared_ptr<tmux::Client> m_client;
    QSocketNotifier *m_notifier = nullptr;
    int m_sessionId = -1;
    bool m_attached = false;
    bool m_detaching = false;
    bool m_finished = false;
    bool m_built = false;                  // the initial lists were applied
    bool m_floatingNoticed = false;
    QStringList m_pendingWindows;          // the initial list-windows, waiting for list-panes
    QHash<int, QString> m_paneCwd;
    QHash<int, int> m_activePane;          // window → tmux's active pane (as we last know or set it)
    QHash<int, int> m_paneWindow;          // pane → its window (from the layouts)
    int m_cols = 0, m_rows = 0, m_sentCols = -1, m_sentRows = -1;
    QTimer m_sizeTimer;
    QTimer m_detachTimer;
    QString m_error;
};
