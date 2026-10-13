// Qt key events → tmux send-keys input (native/tmux/keys.h; docs/solidterm-tmux-control-mode.md
// 3.3). Every name produced here was checked against a real tmux 3.7c (cat -v in a pane): an
// unknown name is TYPED as text by tmux, so the table may only emit names tmux knows — that is why
// Ctrl+Backspace is C-h and Ctrl+Escape drops the Ctrl (3.7c has no C-BSpace / C-Escape).
#include "../native/tmux/keys.h"

#include <cstdio>
#include <string>

namespace {
int g_failures = 0;

void expect(int key, Qt::KeyboardModifiers mods, const std::u32string &text, tmux::KeyInput::Kind kind, const std::string &want)
{
    const tmux::KeyInput in = tmux::keyInput(key, mods, text.data(), int(text.size()));
    if (in.kind != kind || in.text != want) {
        std::fprintf(stderr, "FAIL: key 0x%x mods 0x%x: got kind %d '%s', want kind %d '%s'\n", key, unsigned(mods.toInt()),
                     int(in.kind), in.text.c_str(), int(kind), want.c_str());
        ++g_failures;
    }
}
} // namespace

int main()
{
    using K = tmux::KeyInput;
    const auto N = Qt::NoModifier, S = Qt::ShiftModifier, C = Qt::ControlModifier, A = Qt::AltModifier;
    // Text
    expect(Qt::Key_A, N, U"a", K::Literal, "a");
    expect(Qt::Key_A, S, U"A", K::Literal, "A");
    expect(Qt::Key_unknown, N, U"é", K::Literal, "\xc3\xa9");
    expect(Qt::Key_Space, N, U" ", K::Literal, " ");
    expect(Qt::Key_Apostrophe, N, U"'", K::Literal, "'");
    expect(Qt::Key_Minus, N, U"-", K::Literal, "-");
    expect(Qt::Key_5, Qt::KeypadModifier, U"5", K::Literal, "5");
    // Special keys, plain and with every modifier
    const struct { int key; const char *name; } special[] = {
        { Qt::Key_Return, "Enter" }, { Qt::Key_Enter, "Enter" }, { Qt::Key_Tab, "Tab" }, { Qt::Key_Escape, "Escape" },
        { Qt::Key_Backspace, "BSpace" }, { Qt::Key_Up, "Up" }, { Qt::Key_Down, "Down" }, { Qt::Key_Left, "Left" },
        { Qt::Key_Right, "Right" }, { Qt::Key_Home, "Home" }, { Qt::Key_End, "End" }, { Qt::Key_PageUp, "PPage" },
        { Qt::Key_PageDown, "NPage" }, { Qt::Key_Insert, "IC" }, { Qt::Key_Delete, "DC" }, { Qt::Key_F1, "F1" },
        { Qt::Key_F12, "F12" },
    };
    for (const auto &s : special) {
        const std::string n = s.name;
        expect(s.key, N, U"", K::Name, n);
        expect(s.key, S, U"", K::Name, "S-" + n);
        expect(s.key, A, U"", K::Name, "M-" + n);
        if (s.key != Qt::Key_Backspace && s.key != Qt::Key_Escape) {
            expect(s.key, C, U"", K::Name, "C-" + n);
            expect(s.key, C | S, U"", K::Name, "C-S-" + n);
            expect(s.key, C | A | S, U"", K::Name, "C-M-S-" + n);
        }
    }
    expect(Qt::Key_Backtab, S, U"", K::Name, "BTab");
    expect(Qt::Key_Backspace, C, U"\x08", K::Name, "C-h");
    expect(Qt::Key_Backspace, C | A, U"", K::Name, "C-M-h");
    expect(Qt::Key_Escape, C, U"\x1b", K::Name, "Escape");
    // Chords
    expect(Qt::Key_C, C, U"\x03", K::Name, "C-c");
    expect(Qt::Key_C, C | S, U"\x03", K::Name, "C-c");
    expect(Qt::Key_X, A, U"x", K::Name, "M-x");
    expect(Qt::Key_A, A | S, U"A", K::Name, "M-A");
    expect(Qt::Key_X, C | A, U"\x18", K::Name, "C-M-x");
    expect(Qt::Key_Slash, C, U"\x1f", K::Name, "C-/");
    expect(Qt::Key_Semicolon, A, U";", K::Name, "M-;");
    expect(Qt::Key_Space, C, U" ", K::Name, "C-Space");
    expect(Qt::Key_unknown, A, U"é", K::Name, "M-\xc3\xa9");
    // Nothing
    expect(Qt::Key_Shift, S, U"", K::None, "");
    expect(Qt::Key_Control, C, U"", K::None, "");
    expect(Qt::Key_F20, N, U"", K::None, "");
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("tmux keys: all checks passed");
    return 0;
}
