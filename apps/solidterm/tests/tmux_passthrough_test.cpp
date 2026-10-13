// tmux passthrough unwrapping (native/tmux/passthrough.h): "ESC P tmux; <inner, ESC doubled> ESC \"
// becomes <inner>, everything else passes untouched — for EVERY split of the stream (every single
// cut point, every pair of cut points for short streams, random cuts for long ones), compared with a
// whole-buffer reference.
#include "../native/tmux/passthrough.h"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {
int g_failures = 0;

std::string reference(const std::string &s)
{
    std::string out;
    const std::string open = "\x1bPtmux;";
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, open.size(), open) == 0) {
            i += open.size();
            while (i < s.size()) {
                if (s[i] == '\x1b' && i + 1 < s.size()) {
                    if (s[i + 1] == '\\') { i += 2; break; }
                    if (s[i + 1] == '\x1b') { out += '\x1b'; i += 2; continue; }
                    out += '\x1b'; out += s[i + 1]; i += 2; continue;
                }
                if (s[i] == '\x1b') { ++i; break; } // a trailing lone ESC (unterminated): dropped pending
                out += s[i++];
            }
            continue;
        }
        out += s[i++];
    }
    return out;
}

std::string run(const std::string &s, const std::vector<size_t> &cuts)
{
    tmux::PassthroughUnwrapper u;
    std::string out, buf;
    size_t from = 0;
    std::vector<size_t> c = cuts;
    c.push_back(s.size());
    for (size_t to : c) {
        buf.assign(s, from, to - from);
        std::string before;
        const size_t n = u.process(buf.data(), buf.size(), before);
        out += before;
        out.append(buf.data(), n);
        from = to;
    }
    return out;
}

std::string show(const std::string &s)
{
    std::string o;
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7f) o += char(c);
        else { char b[8]; std::snprintf(b, sizeof b, "\\x%02x", c); o += b; }
    }
    return o;
}

void check(const std::string &s, const std::string &what)
{
    const std::string want = reference(s);
    // every single cut, and every pair of cuts for short inputs
    for (size_t a = 0; a <= s.size(); ++a) {
        const std::string got = run(s, { a });
        if (got != want) {
            std::fprintf(stderr, "FAIL %s (cut %zu)\n  got  %s\n  want %s\n", what.c_str(), a, show(got).c_str(), show(want).c_str());
            ++g_failures;
            return;
        }
        if (s.size() <= 64)
            for (size_t b = a; b <= s.size(); ++b)
                if (run(s, { a, b }) != want) {
                    std::fprintf(stderr, "FAIL %s (cuts %zu,%zu)\n", what.c_str(), a, b);
                    ++g_failures;
                    return;
                }
    }
    // byte by byte
    std::vector<size_t> every;
    for (size_t k = 1; k < s.size(); ++k)
        every.push_back(k);
    if (run(s, every) != want) {
        std::fprintf(stderr, "FAIL %s (byte by byte)\n", what.c_str());
        ++g_failures;
    }
}
} // namespace

int main()
{
    // Hand cases.
    check("plain text", "no escapes");
    check("a\x1b[31mred\x1b[0m b", "SGR untouched");
    check("x\x1bPtmux;\x1b\x1b]52;c;aGk=\x07\x1b\\y", "OSC 52 inside passthrough (BEL-terminated)");
    check("x\x1bPtmux;\x1b\x1b]8;;http://e.x\x1b\x1b\\\x1b\\link\x1bPtmux;\x1b\x1b]8;;\x1b\x1b\\\x1b\\z",
          "OSC 8 hyperlink pair, ST doubled inside");
    check("\x1bPq#0;2;0;0;0#0!10~\x1b\\after", "a sixel DCS (not tmux) is untouched");
    check("\x1bPtmu\x1bPtmux;\x1b\x1b[1mB\x1b\\", "a near-miss prefix, then the real one");
    check("\x1b\x1b\x1bP\x1bPt\x1bPtmux;in\x1b\\", "ESCs back to back");
    check("\x1bPtmux;bad\x1bqmore\x1b\\done", "a lone ESC inside (malformed) is kept");
    // Each must stay byte-exact whatever was before it.
    const std::string expect = reference("x\x1bPtmux;\x1b\x1b]52;c;aGk=\x07\x1b\\y");
    if (expect != "x\x1b]52;c;aGk=\x07y") {
        std::fprintf(stderr, "FAIL reference itself\n");
        ++g_failures;
    }
    // Random streams from these pieces.
    const char *pieces[] = { "abc", "\x1b[0m", "\x1b[38;5;196m", "\xc3\xa9", "\x1bPtmux;\x1b\x1b]52;c;Zm9v\x07\x1b\\",
                             "\x1bPtmux;\x1b\x1b[?25l\x1b\\", "\x1bPq#1\x1b\\", "\x1bP", "\x1bPtm", "\x1b", "\r\n", "\x1b\\" };
    std::mt19937 rng(7);
    for (int t = 0; t < 400; ++t) {
        std::string s;
        const int n = 1 + int(rng() % 12);
        for (int k = 0; k < n; ++k)
            s += pieces[rng() % (sizeof pieces / sizeof *pieces)];
        // the reference treats an unterminated passthrough at the end as dropped: avoid ending inside
        s += "\x1b\\end";
        check(s, "random stream " + std::to_string(t));
        if (g_failures > 5)
            break;
    }
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("tmux passthrough: all checks passed");
    return 0;
}
