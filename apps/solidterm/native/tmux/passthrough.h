#pragma once

// Unwrapping tmux passthrough (docs/solidterm-tmux-control-mode.md 7): programs that know they run
// inside tmux wrap sequences meant for the OUTER terminal as
//     ESC P tmux; <sequence with every ESC doubled> ESC \ (ST)
// (OSC 52 clipboard, OSC 8 links, images…). tmux would unwrap them for a tty client (with
// allow-passthrough); a control client gets the raw bytes in %output, and libvterm would swallow
// the whole DCS. In tmux mode the reader unwraps them in stream order, so the inner sequence reaches
// the pane's parser exactly where the program put it.
//
// Streaming, in place (the result is never longer than the input), any split of the stream. Only a
// pattern prefix (≤ 6 bytes) can be held across calls; when it turns out not to be "ESC P tmux;", the
// held bytes are handed back through `before` (to be written ahead of this call's data).

#include <cstddef>
#include <cstring>
#include <string>

namespace tmux {

class PassthroughUnwrapper {
public:
    static constexpr char kOpen[] = "\x1bPtmux;";
    static constexpr size_t kOpenLen = 7;

    // Unwrap d[0, n) in place; returns the new length. `before` receives bytes to emit before it.
    size_t process(char *d, size_t n, std::string &before)
    {
        size_t r = 0, w = 0;
        while (r < n) {
            if (m_inside) {
                const char c = d[r++];
                if (m_esc) {
                    m_esc = false;
                    if (c == '\\') { // ESC \ : the end of the passthrough
                        m_inside = false;
                        continue;
                    }
                    if (c == '\x1b') { // a doubled ESC: one ESC of the inner sequence
                        d[w++] = '\x1b';   // two bytes read for it: never overtakes the reads
                        continue;
                    }
                    // Malformed (a lone ESC inside): keep both bytes. Two bytes out for two read,
                    // unless the ESC was read by the previous call (w == 0 at the start of this
                    // one): then both go ahead of this call's data.
                    if (w + 2 <= r) {
                        d[w++] = '\x1b';
                        d[w++] = c;
                    } else {
                        before += '\x1b';
                        before += c;
                    }
                    continue;
                }
                if (c == '\x1b') {
                    m_esc = true;
                    continue;
                }
                d[w++] = c;
                continue;
            }
            if (m_match == 0) {
                // Fast path: copy up to the next ESC.
                const char *e = static_cast<const char *>(std::memchr(d + r, '\x1b', n - r));
                const size_t end = e ? size_t(e - d) : n;
                if (w != r && end > r)
                    std::memmove(d + w, d + r, end - r);
                w += end - r;
                r = end;
                if (r == n)
                    break;
                m_match = 1; // the ESC, held
                ++r;
                continue;
            }
            if (d[r] == kOpen[m_match]) {
                ++m_match;
                ++r;
                if (m_match == kOpenLen) { // "ESC P tmux;" — inside from here
                    m_inside = true;
                    m_match = 0;
                    m_carried = 0;
                }
                continue;
            }
            // Not ours (an SGR, another DCS…): give back what was held, then look at d[r] afresh.
            if (m_carried) {
                before.append(kOpen, m_carried); // w == 0 here: nothing of this call is out yet
            }
            for (size_t k = m_carried; k < m_match; ++k)
                d[w++] = kOpen[k];
            m_match = 0;
            m_carried = 0;
        }
        if (m_match)
            m_carried = m_match; // all held bytes now belong to the past
        return w;
    }

    bool inside() const { return m_inside; }

private:
    size_t m_match = 0;   // bytes of kOpen matched (and held)
    size_t m_carried = 0; // of those, how many were held from an earlier call
    bool m_inside = false;
    bool m_esc = false;   // inside, after an ESC
};

} // namespace tmux
