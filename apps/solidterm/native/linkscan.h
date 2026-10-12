#pragma once

#include <cctype>
#include <string>
#include <vector>

// Pure (Qt-free) URL-detection scanner shared between TerminalView::linkAt and its test
// (linkscan_test.cpp) — recognises http/https/ftp/file/mailto URLs and bare "www." links the way
// VTE does: a run of non-space bytes starting at a recognised scheme marker, trimmed of trailing
// punctuation that is clearly NOT part of the URL — including an unmatched closing bracket, kept
// only when an earlier matching opener makes it part of the URL (Wikipedia's
// "...Foo_(disambiguation)"), trimmed otherwise. ASCII only: terminalview.cpp already only looks at
// each cell's first codepoint when it is ASCII (URLs don't use the rest), so QString rows convert
// losslessly for this (1 QChar == 1 byte here).
//
// Row joining (soft wrap): when a URL is split across two terminal rows by the hard column wrap
// (not a real newline), TerminalView::linkAt concatenates the current row's text with however many
// FOLLOWING rows are flagged as a "continuation" (VTermLineInfo::continuation, live rows only — see
// the comment there for the scrollback limitation) BEFORE calling scanLinks, so a match here can
// legitimately span what were two separate screen rows. This header only sees the already-joined
// string; it has no notion of "row" at all.

struct LinkMatch { size_t start = 0; size_t end = 0; }; // [start, end) byte offsets into the scanned text

namespace linkscan_detail {

inline bool isSchemeStart(const std::string &s, size_t i, std::string &schemeOut)
{
    static const char *const kSchemes[] = { "https://", "http://", "ftp://", "file://", "mailto:" };
    for (const char *scheme : kSchemes) {
        const size_t len = std::char_traits<char>::length(scheme);
        if (s.compare(i, len, scheme) == 0) {
            schemeOut.assign(scheme);
            return true;
        }
    }
    if (s.compare(i, 4, "www.") == 0) {
        schemeOut = "www.";
        return true;
    }
    return false;
}

// Trim trailing punctuation that is clearly not part of the URL. A closing bracket is kept when an
// EARLIER matching opener appears inside the candidate match (then the bracket IS part of the URL —
// e.g. a Wikipedia disambiguation link); otherwise it is trimmed like any other trailing punctuation.
inline size_t trimTrailingPunctuation(const std::string &s, size_t start, size_t end)
{
    static const std::string kAlwaysTrim = ").,;:!?\"'>";
    while (end > start) {
        const char c = s[end - 1];
        char open = 0;
        if (c == ')') open = '(';
        else if (c == ']') open = '[';
        else if (c == '}') open = '{';
        if (open) {
            // Count opens/closes BEFORE this trailing one (end-1 is the candidate itself — excluded,
            // or an exactly-matched pair would always cancel to depth 0 and never be kept).
            int depth = 0;
            for (size_t i = start; i + 1 < end; ++i) {
                if (s[i] == open) ++depth;
                else if (s[i] == c) --depth;
            }
            if (depth > 0)
                break; // balanced inside the match — this bracket IS part of the URL
            --end;
            continue;
        }
        if (kAlwaysTrim.find(c) != std::string::npos) {
            --end;
            continue;
        }
        break;
    }
    return end;
}

} // namespace linkscan_detail

// Scans `line` for link-shaped substrings — a scheme marker may start anywhere, not just at a
// whitespace boundary (he said "https://example.com" aloud — the marker follows a bare quote, no
// space), but the match itself always ends at the next whitespace (then gets trailing punctuation
// trimmed). Returns [start,end) byte ranges, left to right, non-overlapping.
inline std::vector<LinkMatch> scanLinks(const std::string &line)
{
    std::vector<LinkMatch> out;
    const size_t n = line.size();
    size_t i = 0;
    while (i < n) {
        std::string scheme;
        if (!std::isspace(static_cast<unsigned char>(line[i])) && linkscan_detail::isSchemeStart(line, i, scheme)) {
            size_t end = i;
            while (end < n && !std::isspace(static_cast<unsigned char>(line[end])))
                ++end;
            end = linkscan_detail::trimTrailingPunctuation(line, i, end);
            if (end > i + scheme.size()) { // more than just the bare scheme marker
                out.push_back({ i, end });
                i = end;
                continue;
            }
        }
        ++i; // no match starting here — just advance one byte (a scheme can start mid-token)
    }
    return out;
}
