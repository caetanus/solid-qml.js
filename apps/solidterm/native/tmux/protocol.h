#pragma once

// tmux control-mode protocol (docs/solidterm-tmux-control-mode.md, section 1). Qt-free.
//
//   ControlParser  — streaming line parser over what the control client's stdout delivers: partial
//                    reads, %begin/%end/%error blocks, %output/%extended-output decoded IN PLACE (the
//                    read buffer is the reusable buffer: a decoded payload is never longer than its
//                    encoding), every other notification split into typed fields.
//   CommandQueue   — the correlator: flags-1 blocks answer our commands strictly in send order; a
//                    command line is a GROUP and an %error drops the group's remaining commands
//                    (tmux removes them without running them, so they never answer).
//   CommandLine    — builds one command line with tmux's quoting rules (cmd-parse.y): every
//                    argument single-quoted, a ' inside spelled '"'"'.
//
// Zero abstraction on the %output path: the parser is a template over its handler (direct,
// inlinable calls, no virtuals, no std::function); one memchr per line, one memchr per escape.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace tmux {

// ─── decoding ───────────────────────────────────────────────────────────────────────────────────

inline bool isOctal(char c) { return c >= '0' && c <= '7'; }

// Decode tmux's escaping IN PLACE and return the decoded length: "\ooo" (three octal digits) → that
// byte (%output encodes every byte < 0x20 and the backslash this way, control.c); "\\" → '\'
// (capture-pane -C writes a backslash so, grid.c); anything else is copied as is.
inline size_t decodeInPlace(char *s, size_t n)
{
    char *w = static_cast<char *>(std::memchr(s, '\\', n));
    if (!w)
        return n;
    const char *r = w;
    const char *const end = s + n;
    while (r < end) {
        // r points at a backslash here.
        const size_t left = size_t(end - r);
        if (left >= 4 && isOctal(r[1]) && isOctal(r[2]) && isOctal(r[3])) {
            *w++ = char(((r[1] - '0') << 6) | ((r[2] - '0') << 3) | (r[3] - '0'));
            r += 4;
        } else if (left >= 2 && r[1] == '\\') {
            *w++ = '\\';
            r += 2;
        } else {
            *w++ = *r++; // a lone backslash: keep it
        }
        // Copy the run up to the next backslash.
        const char *nb = static_cast<const char *>(std::memchr(r, '\\', size_t(end - r)));
        const char *runEnd = nb ? nb : end;
        const size_t run = size_t(runEnd - r);
        if (run) {
            std::memmove(w, r, run);
            w += run;
            r = runEnd;
        }
    }
    return size_t(w - s);
}

inline std::string decoded(std::string_view s)
{
    std::string out(s);
    out.resize(decodeInPlace(out.data(), out.size()));
    return out;
}

// ─── ids and small number parsing ───────────────────────────────────────────────────────────────

// Parse an unsigned decimal at s[i..]; advances i. Returns false when no digit is there.
inline bool parseUnsigned(std::string_view s, size_t &i, uint64_t &out)
{
    const size_t start = i;
    uint64_t v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        v = v * 10 + uint64_t(s[i++] - '0');
    out = v;
    return i > start;
}

// "%12" / "@3" / "$0" → 12 / 3 / 0 (the sigil must match); -1 otherwise.
inline int parseId(std::string_view s, char sigil)
{
    if (s.size() < 2 || s[0] != sigil)
        return -1;
    size_t i = 1;
    uint64_t v = 0;
    if (!parseUnsigned(s, i, v) || i != s.size() || v > 0x7fffffff)
        return -1;
    return int(v);
}

// ─── notifications ──────────────────────────────────────────────────────────────────────────────

enum class Note : uint8_t {
    Unknown,
    Pause,                  // %pause %P
    Continue,               // %continue %P
    LayoutChange,           // %layout-change @W <layout> <visible-layout> <raw-flags>
    WindowAdd,              // %window-add @W
    WindowClose,            // %window-close @W
    WindowRenamed,          // %window-renamed @W <name…>
    UnlinkedWindowAdd,      // %unlinked-window-add @W
    UnlinkedWindowClose,    // %unlinked-window-close @W  (3.7c also sends this when one of OUR windows is killed)
    UnlinkedWindowRenamed,  // %unlinked-window-renamed @W <name…>
    WindowPaneChanged,      // %window-pane-changed @W %P
    SessionWindowChanged,   // %session-window-changed $S @W
    SessionChanged,         // %session-changed $S <name…>
    SessionRenamed,         // %session-renamed [$S] <name…>  (the man page omits the id; the code sends it)
    SessionsChanged,        // %sessions-changed
    ClientSessionChanged,   // %client-session-changed <client> $S <name…>
    ClientDetached,         // %client-detached <client>
    PaneModeChanged,        // %pane-mode-changed %P
    PasteBufferChanged,     // %paste-buffer-changed <name>
    PasteBufferDeleted,     // %paste-buffer-deleted <name>
    SubscriptionChanged,    // %subscription-changed <name> … : <value>
    Message,                // %message <text…>
    ConfigError,            // %config-error <text…>
    Exit,                   // %exit [reason…]
};

const char *noteName(Note n);

// One notification line, split. The views point into the line and are valid during the callback.
struct Notification {
    Note type = Note::Unknown;
    std::string_view line;  // the whole line
    std::string_view name;  // word after '%' ("layout-change")
    int session = -1, window = -1, pane = -1;
    std::string_view text;  // name / reason / message / layout / client, per type
    std::string_view text2; // LayoutChange: visible layout; ClientSessionChanged: session name; SubscriptionChanged: value
    std::string_view text3; // LayoutChange: raw window flags (may be empty)
};

// Parse a '%' line that is neither a guard nor %output/%extended-output. Unknown notifications
// come back as Note::Unknown with `name` set (they are logged and ignored: future-proof).
void parseNotification(std::string_view line, Notification &out);

// ─── blocks ─────────────────────────────────────────────────────────────────────────────────────

struct BlockGuard {
    int64_t time = 0;
    uint64_t number = 0;
    int flags = 0;          // 1 = the command came from THIS client's input
};

// "%begin|%end|%error <time> <number> <flags>" (the word already matched: `rest` starts after it).
bool parseGuard(std::string_view rest, BlockGuard &out);

// ─── the streaming parser ───────────────────────────────────────────────────────────────────────
//
// Handler requirements (plain member functions, called synchronously from feed()):
//   void onOutput(int pane, char *data, size_t len);                 decoded, may be empty
//   void onBlockBegin(const BlockGuard &g);
//   void onBlockLine(std::string_view line);                         raw (not decoded)
//   void onBlockEnd(const BlockGuard &g, bool error);
//   void onNotification(const Notification &n);
//   void onStray(std::string_view line);                             non-% line outside a block
template <class Handler>
class ControlParser {
public:
    explicit ControlParser(Handler &h) : m_h(h) {}

    // Feed what one read returned. `data` is modified (output is decoded in place).
    void feed(char *data, size_t len)
    {
        char *p = data;
        char *const end = data + len;
        if (!m_carry.empty()) {
            char *nl = static_cast<char *>(std::memchr(p, '\n', len));
            if (!nl) {
                m_carry.append(p, len);
                return;
            }
            m_carry.append(p, size_t(nl - p));
            line(m_carry.data(), m_carry.size());
            m_carry.clear();
            p = nl + 1;
        }
        while (p < end) {
            char *nl = static_cast<char *>(std::memchr(p, '\n', size_t(end - p)));
            if (!nl) {
                m_carry.assign(p, size_t(end - p));
                return;
            }
            line(p, size_t(nl - p));
            p = nl + 1;
        }
    }

    bool inBlock() const { return m_inBlock; }
    const BlockGuard &openBlock() const { return m_block; }
    size_t carried() const { return m_carry.size(); }

private:
    void line(char *s, size_t n)
    {
        if (n && s[n - 1] == '\r') // defensive: a tty in between (the -CC gateway) adds \r
            --n;
        const std::string_view v(s, n);
        if (m_inBlock) {
            // Only the guard carrying the open block's number closes it: an output line that merely
            // looks like "%end …" (captured text) cannot.
            BlockGuard g;
            bool err = false;
            if ((v.substr(0, 5) == "%end " && parseGuard(v.substr(5), g))
                || (err = v.substr(0, 7) == "%error " && parseGuard(v.substr(7), g))) {
                if (g.number == m_block.number) {
                    m_inBlock = false;
                    m_h.onBlockEnd(g, err);
                    return;
                }
            }
            m_h.onBlockLine(v);
            return;
        }
        if (n == 0 || s[0] != '%') {
            m_h.onStray(v);
            return;
        }
        // Hot path first: "%output %P <data>".
        if (n >= 10 && std::memcmp(s, "%output %", 9) == 0) {
            size_t i = 9;
            uint64_t pane;
            if (parseUnsigned(v, i, pane) && (i == n || s[i] == ' ')) {
                char *d = i < n ? s + i + 1 : s + n;
                const size_t dn = size_t(s + n - d);
                m_h.onOutput(int(pane), d, decodeInPlace(d, dn));
                return;
            }
        }
        // "%extended-output %P <age> [<reserved>…] : <data>"
        if (n >= 19 && std::memcmp(s, "%extended-output %", 18) == 0) {
            size_t i = 18;
            uint64_t pane;
            if (parseUnsigned(v, i, pane)) {
                const size_t sep = v.find(" : ", i);
                if (sep != std::string_view::npos) {
                    char *d = s + sep + 3;
                    const size_t dn = size_t(s + n - d);
                    m_h.onOutput(int(pane), d, decodeInPlace(d, dn));
                    return;
                }
                // "%extended-output %P <age> :" with nothing after the colon: empty data.
                if (n >= 2 && v.substr(n - 2) == " :") {
                    m_h.onOutput(int(pane), s + n, 0);
                    return;
                }
            }
        }
        if (v.substr(0, 7) == "%begin ") {
            BlockGuard g;
            if (parseGuard(v.substr(7), g)) {
                m_inBlock = true;
                m_block = g;
                m_h.onBlockBegin(g);
                return;
            }
        }
        Notification note;
        parseNotification(v, note);
        m_h.onNotification(note);
    }

    Handler &m_h;
    std::string m_carry;  // the unfinished last line of the previous read
    bool m_inBlock = false;
    BlockGuard m_block;
};

// ─── the correlator ─────────────────────────────────────────────────────────────────────────────

// One command we sent. `kind`/`tag`/`pane` are the owner's (dispatch on kind, no callbacks).
struct Pending {
    uint16_t kind = 0;
    bool lastInGroup = false;
    uint32_t group = 0;
    int pane = -1;
    uint64_t tag = 0;
};

class CommandQueue {
public:
    // Register the commands of ONE line, in order (assigns the group and marks the last entry).
    void pushGroup(const Pending *entries, size_t n);
    // A flags-1 block ended. Fills `done` with the command it answers and returns true; when
    // `error`, appends to `dropped` the rest of that command's group (tmux removed them unrun, so
    // they will never answer). Returns false when nothing is pending: the stream is desynchronised.
    bool complete(bool error, Pending &done, std::vector<Pending> &dropped);
    bool empty() const { return m_q.empty(); }
    size_t size() const { return m_q.size(); }
    // Desync reset: move everything pending to `out`.
    void clear(std::vector<Pending> &out);

private:
    std::deque<Pending> m_q;
    uint32_t m_nextGroup = 1;
};

// ─── command building ───────────────────────────────────────────────────────────────────────────

// Append `a` as ONE literal tmux argument: single quotes make everything literal (no \, ~, $
// expansion: cmd-parse.y), and a ' itself is closed-quote, "'", reopen — tmux's lexer joins the
// pieces of one token as sh does. Format expansion (#{…}) is the command's business, not the
// parser's, so it still happens where the command does it.
void appendQuoted(std::string &out, std::string_view a);
std::string quoted(std::string_view a);

class CommandLine {
public:
    // Start a command; commands of one line are separated by " ; " (one group).
    CommandLine &cmd(std::string_view name);
    CommandLine &flag(std::string_view f);             // verbatim, e.g. "-p"
    CommandLine &arg(std::string_view a);              // quoted
    CommandLine &flagArg(std::string_view f, std::string_view a) { return flag(f).arg(a); }
    CommandLine &pane(std::string_view f, int id);     // f '%id'
    CommandLine &window(std::string_view f, int id);   // f '@id'
    CommandLine &session(std::string_view f, int id);  // f '$id' (quoted: a bare $0 is a variable)
    CommandLine &number(std::string_view f, long long v); // f <v>

    // False when an argument held a newline or NUL: a line can never carry them (a newline would end
    // the command early; an empty line even detaches the client). Such a line must not be sent.
    bool ok() const { return m_ok; }
    size_t commands() const { return m_commands; }
    const std::string &str() const { return m_line; }

private:
    std::string m_line;
    size_t m_commands = 0;
    bool m_ok = true;
};

} // namespace tmux
