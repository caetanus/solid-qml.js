// Mouse-reporting policy of SolidTerm's TerminalView (xterm/VTE behaviour), without Qt — same
// callback shapes as scrollback_test.cpp, driving the SAME pure helpers TerminalView uses
// (mousereport.h) plus the raw libvterm mouse API (vterm_mouse_move/vterm_mouse_button) the way
// TerminalView::reportMouse{Press,Release,Motion,Wheel} call them. No synthetic byte-building here:
// every escape sequence below is whatever libvterm itself produced for the mode the program asked
// for (DECSET 1000/1002/1006 etc.), via its output callback — the same one the real pty write path
// uses.
#include "../native/mousereport.h"

#include <vterm.h>

#include <cstdio>
#include <string>

namespace {

struct MouseTerm {
    VTerm *vt = nullptr;
    int mouseMode = VTERM_PROP_MOUSE_NONE; // mirrors TerminalView::m_mouseMode
    bool altScreen = false;                // mirrors TerminalView::m_altScreen
    std::string out;                       // bytes the "pty" would have received
};

// Same shape as TerminalView::cbSetTermProp's MOUSE/ALTSCREEN branches.
int settermprop(VTermProp prop, VTermValue *val, void *user)
{
    auto *t = static_cast<MouseTerm *>(user);
    if (prop == VTERM_PROP_MOUSE)
        t->mouseMode = val->number;
    else if (prop == VTERM_PROP_ALTSCREEN)
        t->altScreen = val->boolean;
    return 1;
}

void outputCb(const char *s, size_t len, void *user)
{
    static_cast<MouseTerm *>(user)->out.append(s, len);
}

VTerm *newTerm(MouseTerm &t, VTermScreenCallbacks &callbacks, int rows = 24, int cols = 80)
{
    VTerm *vt = vterm_new(rows, cols);
    vterm_set_utf8(vt, 1);
    t.vt = vt;
    vterm_output_set_callback(vt, outputCb, &t);
    VTermScreen *screen = vterm_obtain_screen(vt);
    callbacks = {};
    callbacks.settermprop = settermprop;
    vterm_screen_set_callbacks(screen, &callbacks, &t);
    vterm_screen_enable_altscreen(screen, 1);
    vterm_screen_reset(screen, 1);
    return vt;
}

bool expectEq(const char *what, const std::string &got, const std::string &want)
{
    if (got == want)
        return true;
    std::fprintf(stderr, "%s: got %zu bytes, want %zu bytes\n", what, got.size(), want.size());
    std::fprintf(stderr, "  got:  "); for (unsigned char c : got) std::fprintf(stderr, "%02x ", c); std::fprintf(stderr, "\n");
    std::fprintf(stderr, "  want: "); for (unsigned char c : want) std::fprintf(stderr, "%02x ", c); std::fprintf(stderr, "\n");
    return false;
}

// ── 1. SGR mode (DECSET 1000 + 1006): a left click press then release at cell (row=5, col=10),
//    0-based — exactly the coordinates TerminalView::reportMousePress/Release compute from a pixel
//    position via pixelToCellIndex(). xterm's SGR encoding is 1-based, so this is row 6, col 11.
bool sgrClickAndRelease()
{
    MouseTerm t;
    VTermScreenCallbacks cb;
    VTerm *vt = newTerm(t, cb);
    vterm_input_write(vt, "\x1b[?1000h\x1b[?1006h", 16);
    if (t.mouseMode != VTERM_PROP_MOUSE_CLICK) {
        std::fprintf(stderr, "sgrClickAndRelease: mouse mode %d, want CLICK\n", t.mouseMode);
        vterm_free(vt);
        return false;
    }

    t.out.clear();
    vterm_mouse_move(vt, 5, 10, VTERM_MOD_NONE);
    vterm_mouse_button(vt, 1, true, VTERM_MOD_NONE); // left press
    const bool press = expectEq("sgrClickAndRelease press", t.out, "\x1b[<0;11;6M");

    t.out.clear();
    vterm_mouse_button(vt, 1, false, VTERM_MOD_NONE); // left release
    const bool release = expectEq("sgrClickAndRelease release", t.out, "\x1b[<0;11;6m");

    vterm_free(vt);
    return press && release;
}

// ── 2. X10 mode (DECSET 1000 WITHOUT 1006): the legacy encoding — "\e[M" + three raw bytes
//    (button+32, col+1+32, row+1+32), capped at 223 (255-32) so it only round-trips up to that
//    many columns/rows — nothing like SGR's decimal "<cb;cx;cy>". We don't hardcode the byte
//    values (that would just be re-deriving xterm's spec by hand); we assert the SHAPE is
//    different from SGR, which is the actual behavioural contract TerminalView relies on: it never
//    builds these bytes itself, libvterm does, per whatever the program asked for.
bool x10ClickNoSgr()
{
    MouseTerm t;
    VTermScreenCallbacks cb;
    VTerm *vt = newTerm(t, cb);
    vterm_input_write(vt, "\x1b[?1000h", 8); // no 1006
    if (t.mouseMode != VTERM_PROP_MOUSE_CLICK) {
        std::fprintf(stderr, "x10ClickNoSgr: mouse mode %d, want CLICK\n", t.mouseMode);
        vterm_free(vt);
        return false;
    }

    t.out.clear();
    vterm_mouse_move(vt, 5, 10, VTERM_MOD_NONE);
    vterm_mouse_button(vt, 1, true, VTERM_MOD_NONE);
    vterm_free(vt);

    // X10: ESC [ M <cb> <cx> <cy> — exactly 6 bytes, no '<' (SGR's marker), no ';' separators.
    const bool shape = t.out.size() == 6 && t.out.rfind("\x1b[M", 0) == 0
        && t.out.find('<') == std::string::npos && t.out.find(';') == std::string::npos;
    if (!shape) {
        std::fprintf(stderr, "x10ClickNoSgr: unexpected shape, %zu bytes: ", t.out.size());
        for (unsigned char c : t.out) std::fprintf(stderr, "%02x ", c);
        std::fprintf(stderr, "\n");
    }
    return shape;
}

// ── 3. DRAG mode (DECSET 1002): motion only reports while a button is down. TerminalView itself
//    (reportMouseMotion) gates the vterm_mouse_move() call on `buttons != NoButton` for DRAG — this
//    proves libvterm's OWN filtering agrees (defence in depth: even if our gate had a bug, libvterm
//    still wouldn't emit a motion report with no button down in CLICK/DRAG mode).
bool dragModeMotion()
{
    MouseTerm t;
    VTermScreenCallbacks cb;
    VTerm *vt = newTerm(t, cb);
    vterm_input_write(vt, "\x1b[?1002h\x1b[?1006h", 16);
    if (t.mouseMode != VTERM_PROP_MOUSE_DRAG) {
        std::fprintf(stderr, "dragModeMotion: mouse mode %d, want DRAG\n", t.mouseMode);
        vterm_free(vt);
        return false;
    }

    vterm_mouse_move(vt, 0, 0, VTERM_MOD_NONE);
    vterm_mouse_button(vt, 1, true, VTERM_MOD_NONE); // left down
    t.out.clear();
    vterm_mouse_move(vt, 1, 2, VTERM_MOD_NONE); // motion WITH a button down → reported
    const bool motionReported = !t.out.empty();
    if (!motionReported)
        std::fprintf(stderr, "dragModeMotion: expected a motion report while button 1 is down, got none\n");

    vterm_mouse_button(vt, 1, false, VTERM_MOD_NONE); // release
    t.out.clear();
    vterm_mouse_move(vt, 2, 3, VTERM_MOD_NONE); // motion with NO button down → DRAG mode: nothing
    const bool noMotionWithoutButton = t.out.empty();
    if (!noMotionWithoutButton)
        std::fprintf(stderr, "dragModeMotion: expected no report with no button down (DRAG mode), got %zu bytes\n", t.out.size());

    vterm_free(vt);
    return motionReported && noMotionWithoutButton;
}

// ── 4. Wheel → SGR buttons 4 (up) / 5 (down): one PRESS per notch, per the task's worked example —
//    a wheel-up notch at the same (row=5, col=10) cell as the click test.
bool wheelUpSgr()
{
    MouseTerm t;
    VTermScreenCallbacks cb;
    VTerm *vt = newTerm(t, cb);
    vterm_input_write(vt, "\x1b[?1000h\x1b[?1006h", 16);

    t.out.clear();
    vterm_mouse_move(vt, 5, 10, VTERM_MOD_NONE);
    vterm_mouse_button(vt, 4, true, VTERM_MOD_NONE); // wheel up, one notch
    const bool ok = expectEq("wheelUpSgr", t.out, "\x1b[<64;11;6M");

    vterm_free(vt);
    return ok;
}

// ── 5. Alternate screen, no mouse mode (a pager: less/man/git log) → wheel becomes cursor Up/Down
//    keys, 3 per notch, through vterm_keyboard_key so DECCKM (application cursor-key mode) is
//    respected — "\e[A" normally, "\eOA" once the program has set DECCKM.
bool altScrollKeys()
{
    MouseTerm t;
    VTermScreenCallbacks cb;
    VTerm *vt = newTerm(t, cb);
    vterm_input_write(vt, "\x1b[?1049h", 8); // enter the alternate screen (no DECSET 1000/1002/1003)
    if (!t.altScreen || t.mouseMode != VTERM_PROP_MOUSE_NONE) {
        std::fprintf(stderr, "altScrollKeys: altScreen=%d mouseMode=%d, want altScreen + NONE\n", t.altScreen, t.mouseMode);
        vterm_free(vt);
        return false;
    }

    t.out.clear();
    for (int i = 0; i < 3; ++i)
        vterm_keyboard_key(vt, VTERM_KEY_UP, VTERM_MOD_NONE);
    const bool normal = expectEq("altScrollKeys (normal cursor keys)", t.out, "\x1b[A\x1b[A\x1b[A");

    vterm_input_write(vt, "\x1b[?1h", 5); // DECSET 1 — application cursor-key mode (DECCKM)
    t.out.clear();
    for (int i = 0; i < 3; ++i)
        vterm_keyboard_key(vt, VTERM_KEY_UP, VTERM_MOD_NONE);
    const bool decckm = expectEq("altScrollKeys (DECCKM)", t.out, "\x1bOA\x1bOA\x1bOA");

    vterm_free(vt);
    return normal && decckm;
}

// ── 6. The decision table itself (decideMouseAction) — the policy TerminalView's mouse handlers
//    consult before doing anything: Shift always wins (no bytes, local behaviour), independent of
//    mode/altscreen.
bool decisionTable()
{
    bool ok = true;
    auto check = [&](const char *what, MouseAction got, MouseAction want) {
        if (got != want) {
            std::fprintf(stderr, "decisionTable %s: got %d want %d\n", what, int(got), int(want));
            ok = false;
        }
    };
    // Shift held → Local no matter what the program asked for.
    check("shift+CLICK", decideMouseAction(VTERM_PROP_MOUSE_CLICK, false, true), MouseAction::Local);
    check("shift+MOVE+altscreen", decideMouseAction(VTERM_PROP_MOUSE_MOVE, true, true), MouseAction::Local);
    // No shift: mouse mode on → Report.
    check("CLICK", decideMouseAction(VTERM_PROP_MOUSE_CLICK, false, false), MouseAction::Report);
    check("DRAG", decideMouseAction(VTERM_PROP_MOUSE_DRAG, false, false), MouseAction::Report);
    check("MOVE+altscreen", decideMouseAction(VTERM_PROP_MOUSE_MOVE, true, false), MouseAction::Report);
    // No shift, no mouse mode, altscreen → AltScroll.
    check("NONE+altscreen", decideMouseAction(VTERM_PROP_MOUSE_NONE, true, false), MouseAction::AltScroll);
    // No shift, no mouse mode, no altscreen → Local (today's scrollback wheel / selection / menu).
    check("NONE", decideMouseAction(VTERM_PROP_MOUSE_NONE, false, false), MouseAction::Local);
    return ok;
}

} // namespace

int main()
{
    const bool a = sgrClickAndRelease();
    const bool b = x10ClickNoSgr();
    const bool c = dragModeMotion();
    const bool d = wheelUpSgr();
    const bool e = altScrollKeys();
    const bool f = decisionTable();
    const bool ok = a && b && c && d && e && f;
    std::puts(ok ? "mousereport ok" : "mousereport FAILED");
    return ok ? 0 : 1;
}
