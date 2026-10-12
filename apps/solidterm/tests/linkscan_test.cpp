// Pure URL-detection scanner (linkscan.h) used by TerminalView::linkAt — same style as
// autoscroll_test.cpp/scrollbar_test.cpp: no Qt, no vterm. A table of strings -> expected matched
// substrings, plus the row-join scenario (a URL split across a soft-wrapped screen row).
#include "../native/linkscan.h"

#include <cstdio>

namespace {

bool expectEq(const char *what, const std::string &got, const std::string &want)
{
    if (got == want)
        return true;
    std::fprintf(stderr, "%s: got '%s', want '%s'\n", what, got.c_str(), want.c_str());
    return false;
}

// Runs scanLinks(line) and compares the matched substrings (not offsets — easier to read/write a
// table with) against `want`, in order.
bool expectMatches(const char *what, const std::string &line, const std::vector<std::string> &want)
{
    const std::vector<LinkMatch> got = scanLinks(line);
    bool ok = got.size() == want.size();
    if (!ok)
        std::fprintf(stderr, "%s: got %zu matches, want %zu\n", what, got.size(), want.size());
    for (size_t i = 0; i < got.size() && i < want.size(); ++i)
        ok &= expectEq(what, line.substr(got[i].start, got[i].end - got[i].start), want[i]);
    return ok;
}

// ── 1. The basic table: schemes, bare www., no false positives on plain text.
bool basicTable()
{
    bool ok = true;
    ok &= expectMatches("https", "see https://example.com/path for more", { "https://example.com/path" });
    ok &= expectMatches("http", "http://example.com", { "http://example.com" });
    ok &= expectMatches("ftp", "get it at ftp://ftp.example.org/file.tar.gz", { "ftp://ftp.example.org/file.tar.gz" });
    ok &= expectMatches("file", "open file:///etc/hosts now", { "file:///etc/hosts" });
    ok &= expectMatches("mailto", "mail root at mailto:root@example.com please", { "mailto:root@example.com" });
    ok &= expectMatches("bare www", "visit www.example.com today", { "www.example.com" });
    ok &= expectMatches("no link", "this line has no link at all, just text.", {});
    ok &= expectMatches("bare scheme only", "https:// is not a link by itself", {});
    ok &= expectMatches("multiple", "see http://a.example and https://b.example too",
        { "http://a.example", "https://b.example" });
    return ok;
}

// ── 2. Trailing punctuation: trimmed when it reads as sentence punctuation, kept when it's a
//    balanced bracket that's actually part of the URL (VTE's rule).
bool trailingPunctuation()
{
    bool ok = true;
    ok &= expectMatches("trailing period", "docs at https://example.com/path.", { "https://example.com/path" });
    ok &= expectMatches("trailing comma", "see https://example.com/a, then https://example.com/b",
        { "https://example.com/a", "https://example.com/b" });
    ok &= expectMatches("trailing quote", "he said \"https://example.com\" aloud", { "https://example.com" });
    ok &= expectMatches("sentence close paren", "(see https://example.com/x)", { "https://example.com/x" });
    ok &= expectMatches("balanced paren kept",
        "https://en.wikipedia.org/wiki/Foo_(disambiguation)",
        { "https://en.wikipedia.org/wiki/Foo_(disambiguation)" });
    ok &= expectMatches("unbalanced bracket trimmed",
        "[https://example.com/path]", { "https://example.com/path" });
    return ok;
}

// ── 3. Row-joined soft wrap: TerminalView::linkAt concatenates a continuation row onto the current
//    one BEFORE calling scanLinks (see linkscan.h's header comment) — this is what that join must
//    produce: a URL that was split mid-string across the 80-column wrap is detected whole once
//    joined, where scanning either half ALONE would miss it (or find only a truncated prefix).
bool wrappedAcrossRows()
{
    bool ok = true;
    const std::string row0 = "fetching https://example.com/some/very/long/path/that/keeps";
    const std::string row1 = "/going?query=1&more=2 and then some trailing words";
    const std::string joined = row0 + row1; // exactly what TerminalView::linkAt builds for a wrap
    ok &= expectMatches("joined finds the whole url", joined,
        { "https://example.com/some/very/long/path/that/keeps/going?query=1&more=2" });

    // Scanning row0 alone (as if the join were skipped) finds only the truncated prefix — the
    // behaviour the join exists to fix, pinned here so a regression shows up as a joined-row failure.
    const std::vector<LinkMatch> unjoined = scanLinks(row0);
    ok &= (unjoined.size() == 1 && unjoined[0].end == row0.size()) ? true : [] {
        std::fprintf(stderr, "wrappedAcrossRows: row0-alone sanity check failed\n");
        return false;
    }();
    return ok;
}

} // namespace

int main()
{
    const bool a = basicTable();
    const bool b = trailingPunctuation();
    const bool c = wrappedAcrossRows();
    const bool ok = a && b && c;
    std::puts(ok ? "linkscan ok" : "linkscan FAILED");
    return ok ? 0 : 1;
}
