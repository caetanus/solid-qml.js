#pragma once

// Qt key events → what SolidTerm sends a tmux pane (docs/solidterm-tmux-control-mode.md 3.3).
//
// tmux is the encoder: it knows the pane's modes (cursor keys, keypad, extended keys) and its TERM's
// terminfo (tmux-256color sends Home as \E[1~, not libvterm's \E[H). So special keys and chords go
// as tmux KEY NAMES (send-keys -t %P <name>), plain text as a LITERAL (send-keys -l), coalesced by
// the caller. Header-only; needs only Qt's key enums.

#include <QtCore/qnamespace.h>

#include <string>

namespace tmux {

struct KeyInput {
    enum Kind { None, Name, Literal } kind = None;
    std::string text; // the key name, or the UTF-8 literal
};

inline const char *specialKeyName(int key)
{
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter: return "Enter";
    case Qt::Key_Backspace: return "BSpace";
    case Qt::Key_Tab: return "Tab";
    case Qt::Key_Backtab: return "BTab";
    case Qt::Key_Escape: return "Escape";
    case Qt::Key_Up: return "Up";
    case Qt::Key_Down: return "Down";
    case Qt::Key_Left: return "Left";
    case Qt::Key_Right: return "Right";
    case Qt::Key_Insert: return "IC";
    case Qt::Key_Delete: return "DC";
    case Qt::Key_Home: return "Home";
    case Qt::Key_End: return "End";
    case Qt::Key_PageUp: return "PPage";
    case Qt::Key_PageDown: return "NPage";
    case Qt::Key_F1: return "F1";
    case Qt::Key_F2: return "F2";
    case Qt::Key_F3: return "F3";
    case Qt::Key_F4: return "F4";
    case Qt::Key_F5: return "F5";
    case Qt::Key_F6: return "F6";
    case Qt::Key_F7: return "F7";
    case Qt::Key_F8: return "F8";
    case Qt::Key_F9: return "F9";
    case Qt::Key_F10: return "F10";
    case Qt::Key_F11: return "F11";
    case Qt::Key_F12: return "F12";
    default: return nullptr;
    }
}

inline void appendUtf8(std::string &out, char32_t c)
{
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xc0 | (c >> 6));
        out += char(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
        out += char(0xe0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3f));
        out += char(0x80 | (c & 0x3f));
    } else {
        out += char(0xf0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3f));
        out += char(0x80 | ((c >> 6) & 0x3f));
        out += char(0x80 | (c & 0x3f));
    }
}

// `key` = QKeyEvent::key(), `mods` = its modifiers, `text`/`textLen` = its text as UCS-4.
inline KeyInput keyInput(int key, Qt::KeyboardModifiers mods, const char32_t *text, int textLen)
{
    KeyInput in;
    const bool ctrl = mods & Qt::ControlModifier, alt = mods & Qt::AltModifier, shift = mods & Qt::ShiftModifier;
    if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta
        || key == Qt::Key_AltGr || key == Qt::Key_CapsLock || key == Qt::Key_NumLock || key == Qt::Key_Super_L
        || key == Qt::Key_Super_R)
        return in;
    std::string prefix;
    if (ctrl)
        prefix += "C-";
    if (alt)
        prefix += "M-";
    if (const char *name = specialKeyName(key)) {
        // tmux 3.7c has no C-BSpace / C-Escape: an unknown name would be TYPED as text. Ctrl+BSpace
        // is ^H as in xterm; Ctrl on Escape is dropped.
        if (ctrl && key == Qt::Key_Backspace) {
            in.kind = KeyInput::Name;
            in.text = alt ? "C-M-h" : "C-h";
            return in;
        }
        if (ctrl && key == Qt::Key_Escape) {
            in.kind = KeyInput::Name;
            in.text = std::string(alt ? "M-" : "") + (shift ? "S-" : "") + "Escape";
            return in;
        }
        if (key == Qt::Key_Backtab) { // Shift is already in the name
            in.kind = KeyInput::Name;
            in.text = prefix + name;
            return in;
        }
        in.kind = KeyInput::Name;
        in.text = prefix + (shift ? "S-" : "") + name;
        return in;
    }
    if (key == Qt::Key_Space && (ctrl || alt)) {
        in.kind = KeyInput::Name;
        in.text = prefix + "Space";
        return in;
    }
    if (ctrl || alt) {
        // A chord: the base character. Ctrl turns text into a control byte, so take the key code
        // for ASCII (letters lowercase: terminals cannot tell C-a from C-A); Alt alone keeps the
        // typed text (M-A for Alt+Shift+a).
        char32_t base = 0;
        if (!ctrl && textLen == 1 && text[0] >= 0x20 && text[0] != 0x7f)
            base = text[0];
        else if (key >= Qt::Key_A && key <= Qt::Key_Z)
            base = char32_t('a' + (key - Qt::Key_A));
        else if (key > 0x20 && key < 0x7f)
            base = char32_t(key);
        else if (textLen == 1 && text[0] >= 0x20 && text[0] != 0x7f)
            base = text[0];
        if (!base)
            return in;
        in.kind = KeyInput::Name;
        in.text = prefix;
        appendUtf8(in.text, base);
        return in;
    }
    // Plain text (also Shift+letter, AltGr compositions, keypad digits).
    for (int k = 0; k < textLen; ++k) {
        if (text[k] < 0x20 || text[k] == 0x7f)
            return KeyInput {}; // a control character with no key name: nothing sensible to send
        appendUtf8(in.text, text[k]);
    }
    if (!in.text.empty())
        in.kind = KeyInput::Literal;
    return in;
}

} // namespace tmux
