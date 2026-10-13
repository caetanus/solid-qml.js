#pragma once

// Rebuilding a pane's state on attach (docs/solidterm-tmux-control-mode.md, 4.2-4.4). Qt-free.
//
// tmux sends a control client a pane's output only from "now"; what is on the screen and in the
// history lives in tmux's grid. We ask for it (display-message for the modes and cursor,
// capture-pane for the text) and SYNTHESISE a byte stream that brings a fresh parser to the same
// state: libvterm then builds rows, wrap flags and the scrollback the normal way, so TermScreen needs
// no new API.

#include <string>
#include <string_view>
#include <vector>

namespace tmux {

// What display-message -p '<snapshotFormat()>' reports (format.c). pane_title comes last: it may
// contain spaces.
struct PaneSnapshot {
    int width = 0, height = 0;
    long historySize = 0;
    int cursorX = 0, cursorY = 0;
    bool cursorVisible = true;
    bool alternate = false;
    int altSavedX = 0, altSavedY = 0;   // only meaningful with `alternate`
    int scrollUpper = 0, scrollLower = 0;
    bool keypadCursor = false, keypad = false, insert = false, wrap = true, origin = false;
    bool mouseStandard = false, mouseButton = false, mouseAny = false, mouseSgr = false, mouseUtf8 = false;
    bool bracketPaste = false;
    int cursorShape = 0;                // 0 default, 1 block, 2 underline, 3 bar
    bool cursorBlinking = false;
    bool inMode = false;                // a tmux mode (copy mode…) is drawn over the pane
    std::string title;
};

const char *snapshotFormat();
bool parseSnapshot(std::string_view line, PaneSnapshot &out);

// capture-pane replies, raw (still -C escaped) as the block delivered them.
struct SeedCapture {
    std::vector<std::string> history;   // alternate screen only: the history lines (capture -S -N -E -1)
    std::vector<std::string> normal;    // history + screen (-J), or with `alternate` the saved normal screen
    std::vector<std::string> screen;    // the alternate screen, or the visible screen for a reseed
    std::vector<std::string> pending;   // capture-pane -P: an unfinished escape sequence (usually one empty line)
};

// The stream for a FRESH parser of the pane's size: reset, history and screen, cursor, modes, title,
// then the unfinished sequence tmux's parser is in the middle of.
void buildSeed(std::string &out, const PaneSnapshot &s, const SeedCapture &c);

// After a %pause (output in between is lost to us): keep our scrollback, push the stale screen into
// it under a marker row, then paint the fresh screen and restore the modes.
void buildReseed(std::string &out, const PaneSnapshot &s, const SeedCapture &c);

extern const char kReseedMarker[];

} // namespace tmux
