#include "seed.h"

#include "protocol.h"

#include <algorithm>
#include <cstdlib>

namespace tmux {

const char kReseedMarker[] = "-- output skipped while paused (it is in tmux's history) --";

const char *snapshotFormat()
{
    return "#{pane_width} #{pane_height} #{history_size} #{cursor_x} #{cursor_y} #{cursor_flag} "
           "#{alternate_on} #{alternate_saved_x} #{alternate_saved_y} #{scroll_region_upper} "
           "#{scroll_region_lower} #{keypad_cursor_flag} #{keypad_flag} #{insert_flag} #{wrap_flag} "
           "#{origin_flag} #{mouse_standard_flag} #{mouse_button_flag} #{mouse_any_flag} #{mouse_sgr_flag} "
           "#{mouse_utf8_flag} #{bracket_paste_flag} #{cursor_shape} #{cursor_blinking} #{pane_in_mode} "
           "#{pane_title}";
}

namespace {

constexpr int kFields = 25; // before the title

long toLong(std::string_view s)
{
    long v = 0;
    bool neg = false;
    size_t i = 0;
    if (!s.empty() && s[0] == '-') {
        neg = true;
        i = 1;
    }
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i)
        v = v * 10 + (s[i] - '0');
    return neg ? -v : v;
}

void csi2(std::string &out, int a, int b, char final)
{
    out += "\x1b[";
    out += std::to_string(a);
    out += ';';
    out += std::to_string(b);
    out += final;
}

void decset(std::string &out, int mode, bool on)
{
    out += "\x1b[?";
    out += std::to_string(mode);
    out += on ? 'h' : 'l';
}

void appendDecoded(std::string &out, const std::string &line)
{
    const size_t at = out.size();
    out += line;
    out.resize(at + decodeInPlace(out.data() + at, line.size()));
}

// Lines joined by CR LF with nothing after the last (so the last line is the bottom row).
void appendJoined(std::string &out, const std::vector<std::string> &lines, bool &first)
{
    for (const std::string &l : lines) {
        if (!first)
            out += "\r\n";
        first = false;
        appendDecoded(out, l);
    }
}

// Every screen row at its absolute position (alternate screen / reseed: exact rows, no wrap flags).
void appendRows(std::string &out, const std::vector<std::string> &rows, int height)
{
    for (int r = 0; r < int(rows.size()) && r < height; ++r) {
        csi2(out, r + 1, 1, 'H');
        appendDecoded(out, rows[size_t(r)]);
    }
}

// Modes, scroll region, cursor, title, then the pending bytes — common to seed and reseed.
void appendTail(std::string &out, const PaneSnapshot &s, const SeedCapture &c)
{
    out += "\x1b[0m";
    const bool fullRegion = s.scrollUpper <= 0 && (s.scrollLower <= 0 || s.scrollLower >= s.height - 1);
    if (fullRegion)
        out += "\x1b[r";
    else
        csi2(out, s.scrollUpper + 1, s.scrollLower + 1, 'r');
    decset(out, 1, s.keypadCursor);
    out += s.insert ? "\x1b[4h" : "\x1b[4l";
    decset(out, 7, s.wrap);
    decset(out, 1000, s.mouseStandard);
    decset(out, 1002, s.mouseButton);
    decset(out, 1003, s.mouseAny);
    decset(out, 1005, s.mouseUtf8);
    decset(out, 1006, s.mouseSgr);
    decset(out, 2004, s.bracketPaste);
    out += s.keypad ? "\x1b=" : "\x1b>";
    int scusr = 0;
    switch (s.cursorShape) {
    case 1: scusr = s.cursorBlinking ? 1 : 2; break;
    case 2: scusr = s.cursorBlinking ? 3 : 4; break;
    case 3: scusr = s.cursorBlinking ? 5 : 6; break;
    default: scusr = 0; break;
    }
    out += "\x1b[";
    out += std::to_string(scusr);
    out += " q";
    out += "\x1b]2;";
    for (const char ch : s.title)
        if (static_cast<unsigned char>(ch) >= 0x20 && ch != 0x7f)
            out += ch;
    out += '\a';
    decset(out, 6, s.origin); // DECOM homes the cursor: before the cursor position
    const int row = s.origin ? s.cursorY - s.scrollUpper : s.cursorY;
    csi2(out, row + 1, s.cursorX + 1, 'H');
    decset(out, 25, s.cursorVisible);
    std::string pending;
    for (const std::string &l : c.pending)
        pending += l;
    appendDecoded(out, pending);
}

} // namespace

bool parseSnapshot(std::string_view line, PaneSnapshot &out)
{
    std::string_view f[kFields];
    std::string_view rest = line;
    for (int k = 0; k < kFields; ++k) {
        const size_t sp = rest.find(' ');
        if (sp == std::string_view::npos)
            return false;
        f[k] = rest.substr(0, sp);
        rest = rest.substr(sp + 1);
    }
    out = PaneSnapshot {};
    out.width = int(toLong(f[0]));
    out.height = int(toLong(f[1]));
    out.historySize = toLong(f[2]);
    out.cursorX = int(toLong(f[3]));
    out.cursorY = int(toLong(f[4]));
    out.cursorVisible = f[5] == "1";
    out.alternate = f[6] == "1";
    out.altSavedX = out.alternate ? int(toLong(f[7])) : 0; // 4294967295 when there is none
    out.altSavedY = out.alternate ? int(toLong(f[8])) : 0;
    out.scrollUpper = int(toLong(f[9]));
    out.scrollLower = int(toLong(f[10]));
    out.keypadCursor = f[11] == "1";
    out.keypad = f[12] == "1";
    out.insert = f[13] == "1";
    out.wrap = f[14] == "1";
    out.origin = f[15] == "1";
    out.mouseStandard = f[16] == "1";
    out.mouseButton = f[17] == "1";
    out.mouseAny = f[18] == "1";
    out.mouseSgr = f[19] == "1";
    out.mouseUtf8 = f[20] == "1";
    out.bracketPaste = f[21] == "1";
    out.cursorShape = f[22] == "block" ? 1 : f[22] == "underline" ? 2 : f[22] == "bar" ? 3 : 0;
    out.cursorBlinking = f[23] == "1";
    out.inMode = f[24] == "1";
    out.title.assign(rest);
    return out.width > 0 && out.height > 0;
}

void buildSeed(std::string &out, const PaneSnapshot &s, const SeedCapture &c)
{
    out += "\x1b" "c"; // RIS: a known starting point
    bool first = true;
    if (!s.alternate) {
        appendJoined(out, c.normal, first); // history + screen, -J: re-wraps at this width
    } else {
        appendJoined(out, c.history, first);
        appendJoined(out, c.normal, first); // the saved normal screen
        out += "\x1b[0m";
        csi2(out, s.altSavedY + 1, s.altSavedX + 1, 'H');
        decset(out, 1049, true);            // saves that cursor, switches to a cleared alternate screen
        appendRows(out, c.screen, s.height);
    }
    appendTail(out, s, c);
}

void buildReseed(std::string &out, const PaneSnapshot &s, const SeedCapture &c)
{
    out += '\x18';                          // CAN: abandon whatever sequence the lost output cut
    out += "\x1b[0m\x1b[r";
    decset(out, 1049, false);               // back to the normal screen (no-op when already there)
    csi2(out, 9999, 1, 'H');                // bottom row
    out += "\r\n";
    out += "\x1b[2m";
    // One row, whatever the width (a wrapped marker would read as two lines of output).
    out.append(kReseedMarker, std::min(sizeof kReseedMarker - 1, size_t(s.width > 1 ? s.width - 1 : 1)));
    out += "\x1b[0m";
    for (int k = 0; k < s.height; ++k)      // the old screen and the marker go into the scrollback
        out += "\r\n";
    if (s.alternate) {
        csi2(out, s.altSavedY + 1, s.altSavedX + 1, 'H');
        decset(out, 1049, true);
    }
    appendRows(out, c.screen, s.height);
    appendTail(out, s, c);
}

} // namespace tmux
