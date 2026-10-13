#include "tmuxsession.h"

#include "terminalpanes.h"
#include "terminalview.h"
#include "terminaltabs.h"
#include "tmux/client.h"
#include "tmux/layout.h"

#include "termconfig.h"

#include <QCoreApplication>
#include <QPointer>
#include <QProcess>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSocketNotifier>

namespace {

constexpr int kPauseAfterDefault = 5; // 1.6: a stalled SolidTerm costs a reseed, not a disconnect

QPointer<TermConfig> g_config;
QList<TmuxSession *> g_live;

int pref(const char *key, int fallback)
{
    return g_config ? g_config->getInt(QLatin1String(key), fallback) : fallback;
}

QString prefString(const char *key, const QString &fallback)
{
    return g_config ? g_config->getString(QLatin1String(key), fallback) : fallback;
}

// The environment for one-shot tmux processes: without TMUX (they must not think they are nested).
QProcessEnvironment tmuxEnv()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("TMUX"));
    env.remove(QStringLiteral("TMUX_PANE"));
    return env;
}

const char kWindowFormat[] = "#{window_id} #{window_index} #{window_active} #{window_raw_flags} "
                             "#{window_layout} #{window_visible_layout} #{window_name}";
const char kPaneFormat[] = "#{pane_id} #{window_id} #{pane_active} #{pane_width} #{pane_height} "
                           "#{pane_in_mode} #{pane_mode} #{pane_current_path}";

enum Tag : uint64_t { TagInitial = 1, TagRefresh = 2, TagStep = 3, TagMode = 4 };

// Split off n space-separated fields (no collapsing: tmux leaves #{window_raw_flags} empty for a
// window that is neither current nor last); the rest of the line is `rest`.
bool fields(QStringView line, int n, QList<QStringView> &out, QStringView &rest)
{
    out.clear();
    for (int k = 0; k < n; ++k) {
        const qsizetype sp = line.indexOf(QLatin1Char(' '));
        if (sp < 0) {
            if (k == n - 1) {
                out.append(line);
                rest = QStringView();
                return true;
            }
            return false;
        }
        out.append(line.left(sp));
        line = line.mid(sp + 1);
    }
    rest = line;
    return true;
}

} // namespace

void TmuxSession::setConfig(TermConfig *config)
{
    g_config = config;
    QObject::connect(config, &TermConfig::changed, config, [](const QString &key) {
        if (key == QLatin1String("tmuxPauseAfter") || key == QLatin1String("tmuxHoldCap"))
            for (TmuxSession *s : std::as_const(g_live))
                if (s->m_attached && !s->m_detaching) {
                    tmux::CommandLine l;
                    l.cmd("refresh-client").flag("-f").flag("pause-after=" + std::to_string(pref("tmuxPauseAfter", kPauseAfterDefault)));
                    s->m_client->send(l, tmux::Client::KIgnore);
                    s->m_client->setOverflowCap(size_t(pref("tmuxHoldCap", 8)) << 20);
                }
    });
}

TermConfig *TmuxSession::config()
{
    return g_config;
}

QString TmuxSession::binary()
{
    QString bin = qEnvironmentVariable("SOLIDTERM_TMUX_BIN");
    if (bin.isEmpty())
        bin = prefString("tmuxBinary", QString());
    return bin.isEmpty() ? QStringLiteral("tmux") : bin;
}

bool TmuxSession::versionOk(const QString &line, QString *why)
{
    const QString v = line.trimmed();
    if (!v.startsWith(QLatin1String("tmux "))) {
        if (why)
            *why = QStringLiteral("cannot tell the tmux version (\"%1\")").arg(v);
        return false;
    }
    const QString ver = v.mid(5);
    if (ver == QLatin1String("master"))
        return true;
    const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("^(?:next-)?(\\d+)\\.(\\d+)")).match(ver);
    if (!m.hasMatch()) {
        if (why)
            *why = QStringLiteral("cannot tell the tmux version (\"%1\")").arg(v);
        return false;
    }
    const int major = m.captured(1).toInt(), minor = m.captured(2).toInt();
    if (major > kMinMajor || (major == kMinMajor && minor >= kMinMinor))
        return true;
    if (why)
        *why = QStringLiteral("%1 is too old: SolidTerm needs tmux %2.%3 or newer").arg(v).arg(kMinMajor).arg(kMinMinor);
    return false;
}

TmuxSession *TmuxSession::find(const QString &name)
{
    for (TmuxSession *s : std::as_const(g_live))
        if (s->m_name == name && !s->m_detaching && !s->m_finished && s->m_tabs)
            return s;
    return nullptr;
}

QQuickWindow *TmuxSession::window() const
{
    return m_tabs ? m_tabs->window() : nullptr;
}

TmuxSession::TmuxSession(const QString &name, TerminalTabs *tabs, bool attachOnly)
    : QObject(qApp) // outlives the window: a detach completes after the window is gone
    , m_name(name.isEmpty() ? QStringLiteral("main") : name)
    , m_tabs(tabs)
    , m_client(std::make_shared<tmux::Client>())
{
    m_attachOnly = attachOnly;
    g_live.append(this);
    m_sizeTimer.setSingleShot(true);
    m_sizeTimer.setInterval(30); // one refresh-client -C per 30 ms while the window is dragged (3.4)
    connect(&m_sizeTimer, &QTimer::timeout, this, &TmuxSession::sendSize);
    m_detachTimer.setSingleShot(true);
    m_detachTimer.setInterval(2000); // 5.1: then SIGTERM
    connect(&m_detachTimer, &QTimer::timeout, this, &TmuxSession::finish);
    QMetaObject::invokeMethod(this, &TmuxSession::startAttach, Qt::QueuedConnection);
}

TmuxSession::~TmuxSession()
{
    g_live.removeAll(this);
    delete m_notifier;
    m_notifier = nullptr;
    if (m_client)
        m_client->stop(m_detaching ? 2000 : 0);
}

// Before the control client: `tmux -V` (the version gate, 7) and, unless attaching only,
// `tmux has-session -t =<name>` — when it fails, our new-session CREATES the session, which is then
// marked as SolidTerm's (decision 3: SolidTerm reattaches the sessions it created on start).
void TmuxSession::startAttach()
{
    m_probe = new QProcess(this);
    m_probe->setProcessEnvironment(tmuxEnv());
    m_probe->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_probe, &QProcess::finished, this, &TmuxSession::preflightDone);
    connect(m_probe, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            fail(QStringLiteral("tmux was not found (%1): install tmux %2.%3 or newer, or set its path in Preferences")
                     .arg(binary()).arg(kMinMajor).arg(kMinMinor), false);
    });
    m_probeStep = 0;
    m_probe->start(binary(), { QStringLiteral("-V") });
}

void TmuxSession::preflightDone()
{
    if (m_finished || m_detaching)
        return;
    const QString out = QString::fromLocal8Bit(m_probe->readAll());
    if (m_probeStep == 0) {
        QString why;
        if (!versionOk(out.section(QLatin1Char('\n'), 0, 0), &why)) {
            fail(why, false);
            return;
        }
        if (m_attachOnly) {
            spawn();
            return;
        }
        m_probeStep = 1;
        m_probe->start(binary(), { QStringLiteral("has-session"), QStringLiteral("-t"), QLatin1Char('=') + m_name });
        return;
    }
    m_createdByUs = m_probe->exitCode() != 0;
    spawn();
}

void TmuxSession::fail(const QString &why, bool reattach)
{
    qWarning("solidterm: tmux: %s", qPrintable(why));
    m_error = why;
    if (m_tabs)
        m_tabs->tmuxSessionLost(why, reattach);
    emit banner(why, reattach);
    emit exited(why);
    finish();
}

void TmuxSession::spawn()
{
    // Attach, or create then attach (5.1); -x/-y size a NEW session like our panes area (it is
    // corrected by refresh-client -C as soon as the window is laid out).
    const int cols = m_cols > 0 ? m_cols : 80, rows = m_rows > 0 ? m_rows : 24;
    std::vector<std::string> args;
    if (m_attachOnly)
        args = { "-C", "attach-session", "-t", "=" + m_name.toStdString() };
    else
        args = { "-C", "new-session", "-A", "-s", m_name.toStdString(), "-x", std::to_string(cols), "-y", std::to_string(rows) };
    // A reattach restores as much history as our scrollback keeps (4.2), or what the preference says.
    const int history = pref("tmuxHistory", 0);
    if (history > 0)
        m_client->setHistoryLimit(history);
    else if (m_tabs)
        m_client->setHistoryLimit(m_tabs->scrollbackLimit());
    m_client->setOverflowCap(size_t(pref("tmuxHoldCap", 8)) << 20);
    std::string err;
    if (!m_client->start(binary().toStdString(), args, &err)) {
        fail(QString::fromStdString(err), false);
        return;
    }
    m_notifier = new QSocketNotifier(m_client->guiFd(), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &TmuxSession::onEvents);
}

void TmuxSession::setArea(qreal width, qreal height, qreal cellW, qreal cellH)
{
    if (cellW <= 0 || cellH <= 0)
        return;
    // Owner decision 4: tmux's one-row borders become the panes' headers, so the client is one row
    // shorter than the area and the top panes get the freed row for theirs (2.3).
    const int cols = qMax(2, int(width / cellW));
    const int rows = qMax(2, int(height / cellH) - 1);
    if (cols == m_cols && rows == m_rows)
        return;
    m_cols = cols;
    m_rows = rows;
    if (m_attached)
        m_sizeTimer.start();
}

void TmuxSession::sendSize()
{
    if (!m_attached || m_cols <= 0 || (m_cols == m_sentCols && m_rows == m_sentRows))
        return;
    m_sentCols = m_cols;
    m_sentRows = m_rows;
    tmux::CommandLine l;
    l.cmd("refresh-client").flag("-C").flag(std::to_string(m_cols) + "x" + std::to_string(m_rows));
    m_client->send(l, tmux::Client::KIgnore);
}

void TmuxSession::detach()
{
    if (m_detaching || m_finished)
        return;
    m_detaching = true;
    m_tabs = nullptr;
    if (m_client->started() && m_attached) {
        if (prefString("tmuxCloseAction", QStringLiteral("detach")) == QLatin1String("kill") && m_sessionId >= 0) {
            // Preference: closing the window ends the session (and its programs) instead (6.5).
            tmux::CommandLine l;
            l.cmd("kill-session").session("-t", m_sessionId);
            m_client->send(l, tmux::Client::KIgnore);
        }
        m_client->detach();
        m_detachTimer.start();
    } else {
        finish();
    }
}

QString TmuxSession::windowTitle(const QString &paneTitle) const
{
    QString t = m_name;
    if (!paneTitle.isEmpty())
        t += QStringLiteral(" — ") + paneTitle;
    return t;
}

int TmuxSession::seedsCompleted() const { return m_client->seedsCompleted(); }
int TmuxSession::reseedsCompleted() const { return m_client->reseedsCompleted(); }
void TmuxSession::setTee(bool on) { m_client->setTee(on); }
QByteArray TmuxSession::tee(int pane) const { return QByteArray::fromStdString(m_client->tee(pane)); }

void TmuxSession::sendTestStep(const QString &step)
{
    tmux::CommandLine l;
    l.cmd("display-message").flag("-p").arg(step.toStdString());
    m_client->send(l, tmux::Client::KUser, TagStep);
}

void TmuxSession::sendRaw(const QString &command)
{
    tmux::CommandLine l;
    l.cmd(command.toStdString());
    m_client->send(l, tmux::Client::KUser);
}

void TmuxSession::onEvents()
{
    std::vector<tmux::Event> events;
    m_client->takeEvents(events);
    for (tmux::Event &e : events) {
        handle(e);
        if (m_finished)
            return;
    }
}

void TmuxSession::requestLists(bool initial)
{
    tmux::CommandLine w;
    w.cmd("list-windows").flag("-F").arg(kWindowFormat);
    m_client->send(w, tmux::Client::KListWindows, initial ? TagInitial : TagRefresh);
    if (initial) {
        // The client's first list-panes starts seeding every pane that exists now (4.3).
        tmux::CommandLine p;
        p.cmd("list-panes").flag("-s").flag("-F").arg(kPaneFormat);
        m_client->send(p, tmux::Client::KListPanes, TagInitial);
    }
}

void TmuxSession::handle(tmux::Event &e)
{
    using tmux::Note;
    static const bool trace = qEnvironmentVariableIsSet("SOLIDTERM_TMUX_TRACE");
    if (trace) {
        if (e.type == tmux::Event::Notify)
            qInfo("tmux< %%%s %d/%d/%d %s %s %s", tmux::noteName(e.note), e.session, e.window, e.pane, e.text.c_str(), e.text2.c_str(), e.text3.c_str());
        else if (e.type == tmux::Event::Reply)
            qInfo("tmux< reply kind %d tag %llu%s (%zu lines)", e.kind, (unsigned long long)e.tag, e.error ? " ERROR" : "", e.lines.size());
    }
    switch (e.type) {
    case tmux::Event::Log:
        qInfo("solidterm: %s", e.text.c_str());
        if (e.text.rfind("tmux stderr: ", 0) == 0)
            m_lastStderr = QString::fromStdString(e.text.substr(13));
        return;
    case tmux::Event::Exit: {
        if (!m_detaching)
            qInfo("solidterm: tmux client ended%s%s", e.text.empty() ? "" : ": ", e.text.c_str());
        emit exited(QString::fromStdString(e.text));
        if (m_detaching) {
            finish();
            return;
        }
        if (m_tabs && m_built && m_tabs->tmuxTotalTabs() == 0) {
            // The session ended with its last window (detach-on-destroy): the window closes, as a
            // local window does when its last shell exits (5.1).
            m_tabs->tmuxSessionEnded();
            finish();
            return;
        }
        // The server went away, or someone detached / killed this client (7): keep the panes and
        // what they show, read-only, under a banner offering Reattach.
        QString reason = QString::fromStdString(e.text);
        if (reason.isEmpty() && !m_lastStderr.isEmpty())
            reason = m_lastStderr;
        if (!reason.isEmpty()) {
            lost(reason);
            return;
        }
        // tmux 3.7c says a bare "%exit" both for kill-server and for a detach by someone else
        // (recorded): ask the server whether the session is still there to tell them apart.
        auto *probe = new QProcess(this);
        probe->setProcessEnvironment(tmuxEnv());
        connect(probe, &QProcess::finished, this, [this, probe] {
            lost(probe->exitCode() == 0 ? QStringLiteral("detached (by another client or a command)")
                                        : QStringLiteral("the tmux server is gone"));
        });
        connect(probe, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
            if (err == QProcess::FailedToStart)
                lost(QStringLiteral("the tmux client ended"));
        });
        probe->start(binary(), { QStringLiteral("has-session"), QStringLiteral("-t"), QLatin1Char('=') + m_name });
        return;
    }
    case tmux::Event::Reply: {
        QStringList lines;
        for (const std::string &l : e.lines)
            lines.append(QString::fromStdString(l));
        if (e.error) {
            if (!e.dropped)
                qInfo("solidterm: tmux: %s", qPrintable(lines.value(0)));
            return;
        }
        if (e.kind == tmux::Client::KListWindows) {
            if (e.tag == TagInitial && !m_built) {
                m_pendingWindows = lines;
            } else {
                applyWindowList(lines, false);
                // tmux's state after every notification before this reply: parked views whose pane
                // no window names any more are gone.
                flushParked();
            }
        } else if (e.kind == tmux::Client::KListPanes) {
            for (const QString &l : std::as_const(lines)) {
                QList<QStringView> f;
                QStringView rest;
                if (!fields(l, 7, f, rest))
                    continue;
                const int pane = tmux::parseId(f[0].toString().toStdString(), '%');
                const int window = tmux::parseId(f[1].toString().toStdString(), '@');
                m_paneCwd.insert(pane, rest.toString());
                m_paneMode.insert(pane, f[5] == QLatin1String("1") ? f[6].toString() : QString());
                if (f[2] == QLatin1String("1"))
                    m_activePane.insert(window, pane);
            }
            if (!m_built) {
                m_built = true;
                applyWindowList(m_pendingWindows, true);
                m_pendingWindows.clear();
                applyModes();
            }
        } else if (e.kind == tmux::Client::KUser && (e.tag >> 32) == TagMode) {
            // "#{pane_in_mode} #{pane_mode}" for pane (tag & 0xffffffff)
            const int pane = int(e.tag & 0xffffffffu);
            const QString l = lines.value(0);
            m_paneMode.insert(pane, l.startsWith(QLatin1String("1 ")) ? l.mid(2) : QString());
            applyModes();
        } else if (e.kind == tmux::Client::KUser && e.tag == TagStep) {
            emit stepReplied(lines.value(0));
        }
        return;
    }
    case tmux::Event::Notify:
        break;
    }
    const QString text = QString::fromStdString(e.text);
    switch (e.note) {
    case Note::SessionChanged:
        // We are attached (or switched): (re)build from the lists (5.1).
        m_sessionId = e.session;
        if (!m_attached) {
            m_attached = true;
            if (m_cols > 0)
                sendSize();
            tmux::CommandLine pa;
            pa.cmd("refresh-client").flag("-f").flag("pause-after=" + std::to_string(pref("tmuxPauseAfter", kPauseAfterDefault)));
            m_client->send(pa, tmux::Client::KIgnore);
            if (m_createdByUs && m_sessionId >= 0) { // decision 3: ours to reattach on start
                tmux::CommandLine mark;
                mark.cmd("set-option").session("-t", m_sessionId).flag("@solidterm").flag("1");
                m_client->send(mark, tmux::Client::KIgnore);
            }
            requestLists(true);
        }
        break;
    case Note::LayoutChange:
        if (m_built)
            applyLayout(e.window, text, QString::fromStdString(e.text2), QString::fromStdString(e.text3), false);
        break;
    case Note::WindowAdd:
        // 3.7c sends no layout with it: ask (the reconcile is idempotent).
        if (m_built)
            requestLists(false);
        break;
    case Note::WindowClose:
    case Note::UnlinkedWindowClose: // 3.7c reports OUR killed windows this way too
        if (m_tabs)
            m_tabs->tmuxRemoveTab(e.window);
        if (m_hiddenOrigin.remove(e.window))
            emit hiddenChanged();
        break;
    case Note::PaneModeChanged: { // copy mode etc. (6.6): ask which, for the header badge
        tmux::CommandLine l;
        l.cmd("display-message").flag("-p").pane("-t", e.pane).arg("#{pane_in_mode} #{pane_mode}");
        m_client->send(l, tmux::Client::KUser, (uint64_t(TagMode) << 32) | uint32_t(e.pane));
        break;
    }
    case Note::WindowRenamed:
        if (m_tabs)
            m_tabs->tmuxRenameTab(e.window, text);
        break;
    case Note::SessionWindowChanged:
        if (e.session != m_sessionId)
            break;
        m_serverWindow = e.window;
        if (e.window == m_selectedWindow)
            break; // the echo of our own select-window
        m_selectedWindow = -1;
        if (m_tabs) // another client, a tmux binding, or a window we just created
            m_tabs->tmuxSelectTab(e.window);
        break;
    case Note::WindowPaneChanged:
        if (m_activePane.value(e.window, -1) == e.pane)
            break; // the echo of our own select-pane (or nothing new)
        m_activePane.insert(e.window, e.pane);
        if (m_tabs)
            if (TerminalPanes *p = m_tabs->tmuxTab(e.window, false))
                p->tmuxFocusPane(e.pane);
        break;
    case Note::SessionRenamed:
        m_name = text;
        break;
    case Note::Message:
    case Note::ConfigError:
        qInfo("solidterm: tmux: %s", qPrintable(text));
        break;
    case Note::Unknown:
        qInfo("solidterm: tmux: ignoring %s", qPrintable(text));
        break;
    default:
        break;
    }
}

void TmuxSession::applyWindowList(const QStringList &lines, bool initial)
{
    if (!m_tabs)
        return;
    int activeWindow = -1;
    for (const QString &l : lines) {
        QList<QStringView> f;
        QStringView name;
        if (!fields(l, 6, f, name))
            continue;
        const int window = tmux::parseId(f[0].toString().toStdString(), '@');
        if (window < 0)
            continue;
        if (f[2] == QLatin1String("1"))
            activeWindow = window;
        const bool isNew = m_tabs->tmuxTabIndex(window) < 0;
        applyLayout(window, f[4].toString(), f[5].toString(), f[3].toString(), isNew);
        if (isNew || initial)
            m_tabs->tmuxRenameTab(window, name.toString());
    }
    // The session's current window: on attach, and after a window we created (new-window,
    // break-pane) whose %session-window-changed came before its tab existed.
    if (initial && activeWindow >= 0)
        m_serverWindow = activeWindow;
    if (m_serverWindow >= 0)
        m_tabs->tmuxSelectTab(m_serverWindow);
}

void TmuxSession::beforeInput(void *session, int pane)
{
    auto *self = static_cast<TmuxSession *>(session);
    const int window = self->m_paneWindow.value(pane, -1);
    if (window < 0 || self->m_activePane.value(window, -1) == pane)
        return;
    self->m_activePane.insert(window, pane); // the %window-pane-changed echo then matches: no refocus
    tmux::CommandLine l;
    l.cmd("select-pane").pane("-t", pane);
    self->m_client->send(l, tmux::Client::KIgnore);
}

void TmuxSession::applyLayout(int window, const QString &layout, const QString &visible, const QString &flags, bool focusActive)
{
    if (!m_tabs)
        return;
    tmux::Layout l;
    const tmux::LayoutError err = tmux::parseLayout(layout.toStdString(), l);
    if (err != tmux::LayoutError::None) {
        // A protocol error (1.3/7): log it and re-query.
        qWarning("solidterm: tmux: bad layout for @%d (%s): %s", window, tmux::layoutErrorName(err), qPrintable(layout));
        requestLists(false);
        return;
    }
    if (!l.floating.empty() && !m_floatingNoticed) {
        m_floatingNoticed = true; // owner default: floating panes (3.7) are ignored, with a notice
        qInfo("solidterm: tmux: floating panes are not shown (window @%d)", window);
    }
    int zoomPane = -1, zoomCols = 0, zoomRows = 0;
    if (flags.contains(QLatin1Char('Z'))) {
        tmux::Layout v;
        if (tmux::parseLayout(visible.toStdString(), v) == tmux::LayoutError::None && v.root.type == tmux::LayoutCell::Pane) {
            zoomPane = v.root.pane;
            zoomCols = int(v.root.sx);
            zoomRows = int(v.root.sy);
        }
    }
    const bool hide = m_tabs->tmuxTabIndex(window) < 0 && hideIfPending(window, l.root);
    TerminalPanes *panes = m_tabs->tmuxTab(window, true, hide);
    if (!panes)
        return;
    panes->tmuxApplyLayout(l.root, zoomPane, zoomCols, zoomRows);
    applyModes();
    {
        std::vector<const tmux::LayoutCell *> leaves;
        tmux::layoutLeaves(l.root, leaves);
        for (auto it = m_paneWindow.begin(); it != m_paneWindow.end();)
            it = it.value() == window ? m_paneWindow.erase(it) : std::next(it);
        for (const tmux::LayoutCell *c : leaves)
            m_paneWindow.insert(c->pane, window);
    }
    // Only a new tab follows tmux's active pane here; later layouts keep the user's local focus
    // (tmux learns it lazily, beforeInput). %window-pane-changed still moves it.
    if (focusActive && m_activePane.contains(window))
        panes->tmuxFocusPane(m_activePane.value(window));
}

void TmuxSession::finish()
{
    if (m_finished)
        return;
    m_finished = true;
    m_detachTimer.stop();
    delete m_notifier;
    m_notifier = nullptr;
    m_client->stop(500); // after %exit/EOF the process is gone or going; after the 2 s timeout: SIGTERM
    emit finished();
    deleteLater();
}

// ─── structural actions (docs/solidterm-tmux-control-mode.md 2.5) ─────────────────────────────────

void TmuxSession::send(const tmux::CommandLine &line)
{
    m_client->send(line, tmux::Client::KIgnore); // a failure ("no space for new pane") is logged
}

void TmuxSession::split(int pane, int orient)
{
    if (pane < 0)
        return;
    tmux::CommandLine l;
    // -c: tmux expands the format for the target pane — the new pane starts where that one is.
    l.cmd("split-window").flag(orient == Qt::Horizontal ? "-h" : "-v").pane("-t", pane).flagArg("-c", "#{pane_current_path}");
    send(l);
}

void TmuxSession::newWindow(int afterWindow)
{
    tmux::CommandLine l;
    l.cmd("new-window").flag("-a");
    if (afterWindow >= 0)
        l.window("-t", afterWindow);
    l.flagArg("-c", "#{pane_current_path}");
    send(l);
}

void TmuxSession::selectWindow(int window)
{
    if (window < 0 || window == m_serverWindow)
        return;
    m_selectedWindow = window;
    m_serverWindow = window; // a list reply in flight must not switch us back
    tmux::CommandLine l;
    l.cmd("select-window").window("-t", window);
    send(l);
}

void TmuxSession::zoom(int pane)
{
    if (pane < 0)
        return;
    tmux::CommandLine l;
    l.cmd("resize-pane").flag("-Z").pane("-t", pane);
    send(l);
}

void TmuxSession::resizePane(int pane, bool horizontal, int cells)
{
    tmux::CommandLine l;
    l.cmd("resize-pane").pane("-t", pane).number(horizontal ? "-x" : "-y", cells);
    send(l);
}

void TmuxSession::swapPane(int a, int b)
{
    tmux::CommandLine l;
    l.cmd("swap-pane").pane("-s", a).pane("-t", b);
    send(l);
}

void TmuxSession::movePane(int a, int b, bool horizontal, bool before)
{
    tmux::CommandLine l;
    l.cmd("move-pane").pane("-s", a).pane("-t", b).flag(horizontal ? "-h" : "-v");
    if (before)
        l.flag("-b");
    send(l);
}

void TmuxSession::breakPane(int pane)
{
    tmux::CommandLine l;
    l.cmd("break-pane").pane("-s", pane);
    send(l);
}

void TmuxSession::killPane(int pane)
{
    if (pane < 0)
        return;
    tmux::CommandLine l;
    l.cmd("kill-pane").pane("-t", pane);
    send(l);
}

void TmuxSession::killWindow(int window)
{
    if (window < 0)
        return;
    tmux::CommandLine l;
    l.cmd("kill-window").window("-t", window);
    send(l);
}

void TmuxSession::leaveMode(int pane)
{
    if (pane < 0)
        return;
    tmux::CommandLine l;
    l.cmd("send-keys").flag("-X").pane("-t", pane).arg("cancel");
    send(l);
}

void TmuxSession::renameWindow(int window, const QString &name)
{
    if (window < 0)
        return;
    tmux::CommandLine l;
    l.cmd("rename-window").window("-t", window).arg(name.toStdString());
    send(l);
}

// ─── hidden panes (owner decision 2) ─────────────────────────────────────────────────────────────

void TmuxSession::hidePane(int pane)
{
    const int window = m_paneWindow.value(pane, -1);
    if (pane < 0 || window < 0 || !m_tabs)
        return;
    int panesInWindow = 0;
    for (auto it = m_paneWindow.cbegin(); it != m_paneWindow.cend(); ++it)
        panesInWindow += it.value() == window ? 1 : 0;
    if (panesInWindow <= 1) {
        hideWindow(window);
        return;
    }
    // Out of its window, into one of its own that arrives hidden (hideIfPending).
    m_pendingHide.insert(pane, window);
    tmux::CommandLine l;
    l.cmd("break-pane").flag("-d").pane("-s", pane);
    send(l);
}

void TmuxSession::hideWindow(int window)
{
    if (!m_tabs || window < 0 || m_tabs->tmuxTabIndex(window) < 0)
        return;
    if (!m_hiddenOrigin.contains(window))
        m_hiddenOrigin.insert(window, -1);
    m_tabs->tmuxSetHidden(window, true);
    emit hiddenChanged();
    maybeDetachEmpty();
}

bool TmuxSession::hideIfPending(int window, const tmux::LayoutCell &root)
{
    if (root.type != tmux::LayoutCell::Pane || !m_pendingHide.contains(root.pane))
        return false;
    m_hiddenOrigin.insert(window, m_pendingHide.take(root.pane));
    QMetaObject::invokeMethod(this, &TmuxSession::hiddenChanged, Qt::QueuedConnection);
    return true;
}

void TmuxSession::maybeDetachEmpty()
{
    // Nothing visible left in this OS window: it closes, which detaches (5.1); a reattach shows the
    // hidden ones again.
    if (m_tabs && m_tabs->visibleCount() == 0)
        if (QQuickWindow *w = m_tabs->window())
            QMetaObject::invokeMethod(w, &QWindow::close, Qt::QueuedConnection);
}

QVariantList TmuxSession::hidden() const
{
    QVariantList out;
    if (!m_tabs)
        return out;
    for (int w : m_tabs->tmuxWindows()) {
        if (!m_tabs->tmuxTabHidden(w))
            continue;
        QVariantMap m;
        m[QStringLiteral("window")] = w;
        m[QStringLiteral("title")] = m_tabs->tmuxTabName(w);
        out.append(m);
    }
    return out;
}

void TmuxSession::restoreHidden(int window)
{
    if (!m_tabs || !m_tabs->tmuxTabHidden(window))
        return;
    const int origin = m_hiddenOrigin.value(window, -1);
    int pane = -1, panes = 0;
    for (auto it = m_paneWindow.cbegin(); it != m_paneWindow.cend(); ++it)
        if (it.value() == window) {
            pane = it.key();
            ++panes;
        }
    if (origin >= 0 && panes == 1 && m_tabs->tmuxTabIndex(origin) >= 0 && !m_tabs->tmuxTabHidden(origin)) {
        // A broken-out pane goes back into its window; its own (hidden) window then closes.
        tmux::CommandLine l;
        l.cmd("join-pane").pane("-s", pane).window("-t", origin);
        send(l);
        m_hiddenOrigin.remove(window);
        selectWindow(origin);
        m_tabs->tmuxSelectTab(origin);
    } else {
        m_hiddenOrigin.remove(window);
        m_tabs->tmuxSetHidden(window, false); // shows and selects it
    }
    emit hiddenChanged();
}

// ─── views moving between windows ────────────────────────────────────────────────────────────────

void TmuxSession::parkView(int pane, TerminalView *view)
{
    if (!view)
        return;
    if (qEnvironmentVariableIsSet("SOLIDTERM_TMUX_TRACE"))
        qInfo("tmux: park %%%d view %p", pane, static_cast<void *>(view));
    view->setParentItem(nullptr);
    view->setParent(this);
    if (QPointer<TerminalView> old = m_parked.value(pane); old && old != view)
        old->deleteLater();
    m_parked.insert(pane, view);
    if (!m_flushPending) {
        m_flushPending = true;
        requestLists(false); // its reply settles which parked views are gone
    }
}

TerminalView *TmuxSession::takeView(int pane, TerminalPanes *into)
{
    if (qEnvironmentVariableIsSet("SOLIDTERM_TMUX_TRACE"))
        qInfo("tmux: take %%%d (parked: %d)", pane, int(m_parked.contains(pane)));
    if (TerminalView *v = takeParked(pane))
        return v;
    if (!m_tabs)
        return nullptr;
    for (int w : m_tabs->tmuxWindows()) {
        TerminalPanes *p = m_tabs->tmuxTab(w, false);
        if (p && p != into && p->tmuxView(pane))
            return p->tmuxReleaseView(pane);
    }
    return nullptr;
}

TerminalView *TmuxSession::takeParked(int pane)
{
    QPointer<TerminalView> v = m_parked.take(pane);
    return v.data();
}

void TmuxSession::flushParked()
{
    m_flushPending = false;
    for (const QPointer<TerminalView> &v : std::as_const(m_parked)) {
        if (v) {
            m_paneWindow.remove(v->tmuxPane());
            v->deleteLater(); // the pane left the session: its worker stops, the client closes the pipe
        }
    }
    m_parked.clear();
}

void TmuxSession::applyModes()
{
    if (!m_tabs)
        return;
    for (auto it = m_paneMode.cbegin(); it != m_paneMode.cend(); ++it) {
        const int w = m_paneWindow.value(it.key(), -1);
        if (TerminalPanes *p = w >= 0 ? m_tabs->tmuxTab(w, false) : nullptr)
            p->tmuxSetPaneMode(it.key(), it.value());
    }
}

void TmuxSession::killSession()
{
    if (m_sessionId < 0)
        return;
    tmux::CommandLine l;
    l.cmd("kill-session").session("-t", m_sessionId);
    send(l);
}

void TmuxSession::lost(const QString &reason)
{
    if (m_finished)
        return;
    const QString text = m_built ? QStringLiteral("tmux session \"%1\": %2 — the panes are read-only").arg(m_name, reason)
                                 : QStringLiteral("tmux session \"%1\": %2").arg(m_name, reason);
    if (m_tabs)
        m_tabs->tmuxSessionLost(text, true);
    emit banner(text, true);
    finish();
}
