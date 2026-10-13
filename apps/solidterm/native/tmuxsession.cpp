#include "tmuxsession.h"

#include "terminalpanes.h"
#include "terminaltabs.h"
#include "tmux/client.h"
#include "tmux/layout.h"

#include <QCoreApplication>
#include <QSocketNotifier>

namespace {

constexpr int kPauseAfterSeconds = 5; // 1.6: a stalled SolidTerm costs a reseed, not a disconnect

const char kWindowFormat[] = "#{window_id} #{window_index} #{window_active} #{window_raw_flags} "
                             "#{window_layout} #{window_visible_layout} #{window_name}";
const char kPaneFormat[] = "#{pane_id} #{window_id} #{pane_active} #{pane_width} #{pane_height} "
                           "#{pane_current_path}";

enum Tag : uint64_t { TagInitial = 1, TagRefresh = 2, TagStep = 3 };

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

TmuxSession::TmuxSession(const QString &name, TerminalTabs *tabs)
    : QObject(qApp) // outlives the window: a detach completes after the window is gone
    , m_name(name.isEmpty() ? QStringLiteral("main") : name)
    , m_tabs(tabs)
    , m_client(std::make_shared<tmux::Client>())
{
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
    delete m_notifier;
    m_notifier = nullptr;
    if (m_client)
        m_client->stop(m_detaching ? 2000 : 0);
}

void TmuxSession::startAttach()
{
    QString bin = qEnvironmentVariable("SOLIDTERM_TMUX_BIN");
    if (bin.isEmpty())
        bin = QStringLiteral("tmux");
    // Attach, or create then attach (5.1); -x/-y size a NEW session like our panes area (it is
    // corrected by refresh-client -C as soon as the window is laid out).
    const int cols = m_cols > 0 ? m_cols : 80, rows = m_rows > 0 ? m_rows : 24;
    const std::vector<std::string> args = { "-C", "new-session", "-A", "-s", m_name.toStdString(),
                                            "-x", std::to_string(cols), "-y", std::to_string(rows) };
    if (m_tabs) // a reattach restores as much history as our scrollback keeps (4.2)
        m_client->setHistoryLimit(m_tabs->scrollbackLimit());
    std::string err;
    if (!m_client->start(bin.toStdString(), args, &err)) {
        m_error = QString::fromStdString(err);
        qWarning("solidterm: tmux: %s", qPrintable(m_error));
        emit exited(m_error);
        finish();
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
    if (m_client->started()) {
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
    return t + QStringLiteral(" — read-only (phase 1)");
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
    switch (e.type) {
    case tmux::Event::Log:
        qInfo("solidterm: %s", e.text.c_str());
        return;
    case tmux::Event::Exit:
        if (!m_detaching)
            qInfo("solidterm: tmux client ended%s%s", e.text.empty() ? "" : ": ", e.text.c_str());
        emit exited(QString::fromStdString(e.text));
        if (m_tabs)
            m_tabs->tmuxSessionEnded(); // the server-exited banner is phase 4: the window closes
        finish();
        return;
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
            if (e.tag == TagInitial && !m_built)
                m_pendingWindows = lines;
            else
                applyWindowList(lines, false);
        } else if (e.kind == tmux::Client::KListPanes) {
            for (const QString &l : std::as_const(lines)) {
                QList<QStringView> f;
                QStringView rest;
                if (!fields(l, 5, f, rest))
                    continue;
                const int pane = tmux::parseId(f[0].toString().toStdString(), '%');
                const int window = tmux::parseId(f[1].toString().toStdString(), '@');
                m_paneCwd.insert(pane, rest.toString());
                if (f[2] == QLatin1String("1"))
                    m_activePane.insert(window, pane);
            }
            if (!m_built) {
                m_built = true;
                applyWindowList(m_pendingWindows, true);
                m_pendingWindows.clear();
            }
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
            pa.cmd("refresh-client").flag("-f").flag("pause-after=" + std::to_string(kPauseAfterSeconds));
            m_client->send(pa, tmux::Client::KIgnore);
            requestLists(true);
        }
        break;
    case Note::LayoutChange:
        if (m_built)
            applyLayout(e.window, text, QString::fromStdString(e.text2), QString::fromStdString(e.text3));
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
        break;
    case Note::WindowRenamed:
        if (m_tabs)
            m_tabs->tmuxRenameTab(e.window, text);
        break;
    case Note::SessionWindowChanged:
        if (m_tabs && e.session == m_sessionId)
            m_tabs->tmuxSelectTab(e.window);
        break;
    case Note::WindowPaneChanged:
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
        m_tabs->tmuxTab(window, true);
        if (isNew || initial)
            m_tabs->tmuxRenameTab(window, name.toString());
        applyLayout(window, f[4].toString(), f[5].toString(), f[3].toString());
    }
    if (initial && activeWindow >= 0)
        m_tabs->tmuxSelectTab(activeWindow);
}

void TmuxSession::applyLayout(int window, const QString &layout, const QString &visible, const QString &flags)
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
    TerminalPanes *panes = m_tabs->tmuxTab(window, true);
    if (!panes)
        return;
    panes->tmuxApplyLayout(l.root, zoomPane, zoomCols, zoomRows);
    if (m_activePane.contains(window))
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
