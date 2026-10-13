#include "protocol.h"

namespace tmux {

namespace {

// Split off the next space-separated word of `rest` (no collapsing: "a  b" yields "a", "", "b").
std::string_view nextWord(std::string_view &rest)
{
    const size_t sp = rest.find(' ');
    std::string_view w = rest.substr(0, sp);
    rest = sp == std::string_view::npos ? std::string_view() : rest.substr(sp + 1);
    return w;
}

struct NoteEntry {
    const char *name;
    Note type;
};

constexpr NoteEntry kNotes[] = {
    { "pause", Note::Pause },
    { "continue", Note::Continue },
    { "layout-change", Note::LayoutChange },
    { "window-add", Note::WindowAdd },
    { "window-close", Note::WindowClose },
    { "window-renamed", Note::WindowRenamed },
    { "unlinked-window-add", Note::UnlinkedWindowAdd },
    { "unlinked-window-close", Note::UnlinkedWindowClose },
    { "unlinked-window-renamed", Note::UnlinkedWindowRenamed },
    { "window-pane-changed", Note::WindowPaneChanged },
    { "session-window-changed", Note::SessionWindowChanged },
    { "session-changed", Note::SessionChanged },
    { "session-renamed", Note::SessionRenamed },
    { "sessions-changed", Note::SessionsChanged },
    { "client-session-changed", Note::ClientSessionChanged },
    { "client-detached", Note::ClientDetached },
    { "pane-mode-changed", Note::PaneModeChanged },
    { "paste-buffer-changed", Note::PasteBufferChanged },
    { "paste-buffer-deleted", Note::PasteBufferDeleted },
    { "subscription-changed", Note::SubscriptionChanged },
    { "message", Note::Message },
    { "config-error", Note::ConfigError },
    { "exit", Note::Exit },
};

} // namespace

const char *noteName(Note n)
{
    for (const NoteEntry &e : kNotes)
        if (e.type == n)
            return e.name;
    return "unknown";
}

bool parseGuard(std::string_view rest, BlockGuard &out)
{
    size_t i = 0;
    uint64_t t, num, fl;
    if (!parseUnsigned(rest, i, t) || i >= rest.size() || rest[i++] != ' ')
        return false;
    if (!parseUnsigned(rest, i, num) || i >= rest.size() || rest[i++] != ' ')
        return false;
    if (!parseUnsigned(rest, i, fl))
        return false;
    // tmux may grow extra fields one day: accept anything after a space.
    if (i != rest.size() && rest[i] != ' ')
        return false;
    out.time = int64_t(t);
    out.number = num;
    out.flags = int(fl);
    return true;
}

void parseNotification(std::string_view line, Notification &out)
{
    out = Notification {};
    out.line = line;
    std::string_view rest = line.substr(1); // past '%'
    out.name = nextWord(rest);
    for (const NoteEntry &e : kNotes)
        if (out.name == e.name) {
            out.type = e.type;
            break;
        }
    switch (out.type) {
    case Note::Pause:
    case Note::Continue:
    case Note::PaneModeChanged:
        out.pane = parseId(nextWord(rest), '%');
        break;
    case Note::LayoutChange:
        out.window = parseId(nextWord(rest), '@');
        out.text = nextWord(rest);
        out.text2 = nextWord(rest);
        out.text3 = rest; // raw flags (may be empty or absent)
        break;
    case Note::WindowAdd:
    case Note::WindowClose:
    case Note::UnlinkedWindowAdd:
    case Note::UnlinkedWindowClose:
        out.window = parseId(nextWord(rest), '@');
        break;
    case Note::WindowRenamed:
    case Note::UnlinkedWindowRenamed:
        out.window = parseId(nextWord(rest), '@');
        out.text = rest; // the name, spaces included
        break;
    case Note::WindowPaneChanged:
        out.window = parseId(nextWord(rest), '@');
        out.pane = parseId(nextWord(rest), '%');
        break;
    case Note::SessionWindowChanged:
        out.session = parseId(nextWord(rest), '$');
        out.window = parseId(nextWord(rest), '@');
        break;
    case Note::SessionChanged:
        out.session = parseId(nextWord(rest), '$');
        out.text = rest;
        break;
    case Note::SessionRenamed: {
        // "%session-renamed $S name" (the code) or "%session-renamed name" (the man page).
        std::string_view probe = rest;
        const int id = parseId(nextWord(probe), '$');
        if (id >= 0) {
            out.session = id;
            out.text = probe;
        } else {
            out.text = rest;
        }
        break;
    }
    case Note::ClientSessionChanged:
        out.text = nextWord(rest); // client name
        out.session = parseId(nextWord(rest), '$');
        out.text2 = rest;          // session name
        break;
    case Note::ClientDetached:
        out.text = rest;
        break;
    case Note::PasteBufferChanged:
    case Note::PasteBufferDeleted:
    case Note::Message:
    case Note::ConfigError:
    case Note::Exit:
        out.text = rest;
        break;
    case Note::SubscriptionChanged: {
        out.text = nextWord(rest); // subscription name
        const size_t sep = rest.find(" : ");
        out.text2 = sep == std::string_view::npos ? std::string_view() : rest.substr(sep + 3);
        break;
    }
    case Note::SessionsChanged:
    case Note::Unknown:
        break;
    }
}

// ─── correlator ─────────────────────────────────────────────────────────────────────────────────

void CommandQueue::pushGroup(const Pending *entries, size_t n)
{
    if (n == 0)
        return;
    const uint32_t g = m_nextGroup++;
    if (m_nextGroup == 0)
        m_nextGroup = 1;
    for (size_t i = 0; i < n; ++i) {
        Pending p = entries[i];
        p.group = g;
        p.lastInGroup = i + 1 == n;
        m_q.push_back(p);
    }
}

bool CommandQueue::complete(bool error, Pending &done, std::vector<Pending> &dropped)
{
    if (m_q.empty())
        return false;
    done = m_q.front();
    m_q.pop_front();
    if (error && !done.lastInGroup) {
        // cmdq_remove_group: the rest of this line's commands never run, so never answer.
        while (!m_q.empty() && m_q.front().group == done.group) {
            const bool last = m_q.front().lastInGroup;
            dropped.push_back(m_q.front());
            m_q.pop_front();
            if (last)
                break;
        }
    }
    return true;
}

void CommandQueue::clear(std::vector<Pending> &out)
{
    out.insert(out.end(), m_q.begin(), m_q.end());
    m_q.clear();
}

// ─── command building ───────────────────────────────────────────────────────────────────────────

void appendQuoted(std::string &out, std::string_view a)
{
    out += '\'';
    size_t from = 0;
    for (;;) {
        const size_t q = a.find('\'', from);
        if (q == std::string_view::npos) {
            out.append(a.substr(from));
            break;
        }
        out.append(a.substr(from, q - from));
        out += "'\"'\"'"; // close, a double-quoted ', reopen
        from = q + 1;
    }
    out += '\'';
}

std::string quoted(std::string_view a)
{
    std::string s;
    appendQuoted(s, a);
    return s;
}

CommandLine &CommandLine::cmd(std::string_view name)
{
    if (m_commands++)
        m_line += " ; ";
    m_line.append(name);
    return *this;
}

CommandLine &CommandLine::flag(std::string_view f)
{
    m_line += ' ';
    m_line.append(f);
    return *this;
}

CommandLine &CommandLine::arg(std::string_view a)
{
    if (a.find('\n') != std::string_view::npos || a.find('\0') != std::string_view::npos)
        m_ok = false;
    m_line += ' ';
    appendQuoted(m_line, a);
    return *this;
}

CommandLine &CommandLine::pane(std::string_view f, int id)
{
    flag(f);
    m_line += " '%";
    m_line += std::to_string(id);
    m_line += '\'';
    return *this;
}

CommandLine &CommandLine::window(std::string_view f, int id)
{
    flag(f);
    m_line += " '@";
    m_line += std::to_string(id);
    m_line += '\'';
    return *this;
}

CommandLine &CommandLine::session(std::string_view f, int id)
{
    flag(f);
    m_line += " '$";
    m_line += std::to_string(id);
    m_line += '\'';
    return *this;
}

CommandLine &CommandLine::number(std::string_view f, long long v)
{
    flag(f);
    m_line += ' ';
    m_line += std::to_string(v);
    return *this;
}

} // namespace tmux
