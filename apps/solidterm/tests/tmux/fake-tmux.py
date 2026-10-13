#!/usr/bin/env python3
"""A scripted FAKE tmux control-mode server, for SolidTerm's tmux tests (no tmux needed).

Run in place of `tmux` (SOLIDTERM_TMUX_BIN): it ignores its argv (`-C new-session -A -s NAME …`),
speaks tmux 3.7c's control protocol on stdin/stdout (the shapes recorded in real-3.7c-session.txt)
and plays a SCENARIO: a Python file (FAKE_TMUX_SCENARIO) defining `setup(srv)` that builds the
session model and registers reactions to commands. Every command line it receives is appended to
FAKE_TMUX_LOG (when set), so a test can check what SolidTerm sent.

Implemented commands: refresh-client (-C WxH resizes every window and sends %layout-change, -f
pause-after=N switches to %extended-output, -A '%P:continue|pause'), list-windows -F, list-panes -s
-F, display-message -p [-t %P] <format>, capture-pane -p -t %P (-e -C -J -N -a -q -P -S -E), and
detach-client (%exit, then exit). Anything else answers "parse error: unknown command" with
%error, like tmux. One block per command; a failing command drops the rest of its line (tmux's
command groups).
"""
import os
import re
import runpy
import sys
import time

OUT = sys.stdout.buffer


def emit(line: bytes):
    OUT.write(line + b"\n")
    OUT.flush()


def encode_output(data: bytes) -> bytes:
    # control.c: every byte < 0x20 and the backslash as \ooo; everything else as is.
    out = bytearray()
    for b in data:
        if b < 0x20 or b == 0x5C:
            out += b"\\%03o" % b
        else:
            out.append(b)
    return bytes(out)


def encode_capture(data: bytes) -> bytes:
    # capture-pane -C (grid.c): non-printables as \ooo, the backslash as \\.
    out = bytearray()
    for b in data:
        if b == 0x5C:
            out += b"\\\\"
        elif b < 0x20:
            out += b"\\%03o" % b
        else:
            out.append(b)
    return bytes(out)


def checksum(body: str) -> str:
    c = 0
    for ch in body.encode():
        c = ((c >> 1) + ((c & 1) << 15)) & 0xFFFF
        c = (c + ch) & 0xFFFF
    return "%04x" % c


class Pane:
    def __init__(self, pid):
        self.id = pid
        self.w = 80
        self.h = 24
        self.history = []          # list[bytes], oldest first (already SGR-coded, raw bytes)
        self.screen = []           # list[bytes], one per row (fewer = blank rows below)
        self.saved = []            # alternate on: the saved normal screen
        self.alt = False
        self.cursor = (0, 0)
        self.title = "fake"
        self.pending = b""         # capture-pane -P
        self.paused = False
        self.path = "/tmp"
        self.modes = {}            # format name -> value override


class Window:
    def __init__(self, wid, index, name, tree):
        self.id = wid
        self.index = index
        self.name = name
        self.tree = tree           # int pane id | ("{"|"[", [children])
        self.zoom = None           # pane id when zoomed


class Server:
    def __init__(self):
        self.session = 1
        self.session_name = "fake"
        self.windows = []
        self.panes = {}
        self.active_window = None
        self.cols, self.rows = 80, 24
        self.number = 1000
        self.extended = False
        self.reactions = []        # (regex, fn, before, once)
        self.log_path = os.environ.get("FAKE_TMUX_LOG")
        self.exiting = False
        self.capture_groups = 0

    # ── model ────────────────────────────────────────────────────────────────────────────────
    def pane(self, pid, **kw):
        p = self.panes.get(pid) or Pane(pid)
        for k, v in kw.items():
            setattr(p, k, v)
        self.panes[pid] = p
        return p

    def window(self, wid, name, tree, index=None):
        w = Window(wid, len(self.windows) if index is None else index, name, tree)
        self.windows.append(w)
        if self.active_window is None:
            self.active_window = wid
        for pid in self.leaves(tree):
            self.pane(pid)
        self.relayout(w)
        return w

    def find_window(self, wid):
        for w in self.windows:
            if w.id == wid:
                return w
        return None

    def leaves(self, tree):
        if isinstance(tree, int):
            return [tree]
        out = []
        for c in tree[1]:
            out += self.leaves(c)
        return out

    def window_of(self, pid):
        for w in self.windows:
            if pid in self.leaves(w.tree):
                return w
        return None

    def cell(self, tree, x, y, w, h):
        """Lay `tree` out at (x,y,w,h) like tmux (1-cell borders, even split, remainder to the last
        child); returns the layout body and sets the panes' sizes."""
        if isinstance(tree, int):
            p = self.pane(tree)
            p.w, p.h = w, h
            return "%dx%d,%d,%d,%d" % (w, h, x, y, tree)
        kind, children = tree
        n = len(children)
        parts = []
        if kind == "{":
            avail = w - (n - 1)
            pos = x
            for i, c in enumerate(children):
                cw = avail // n if i < n - 1 else avail - (avail // n) * (n - 1)
                parts.append(self.cell(c, pos, y, cw, h))
                pos += cw + 1
            return "%dx%d,%d,%d{%s}" % (w, h, x, y, ",".join(parts))
        avail = h - (n - 1)
        pos = y
        for i, c in enumerate(children):
            ch = avail // n if i < n - 1 else avail - (avail // n) * (n - 1)
            parts.append(self.cell(c, x, pos, w, ch))
            pos += ch + 1
        return "%dx%d,%d,%d[%s]" % (w, h, x, y, ",".join(parts))

    def relayout(self, w):
        body = self.cell(w.tree, 0, 0, self.cols, self.rows)
        w.layout = checksum(body) + "," + body
        if w.zoom is not None:
            z = self.pane(w.zoom)
            z.w, z.h = self.cols, self.rows
            vbody = "%dx%d,0,0,%d" % (self.cols, self.rows, w.zoom)
            w.visible = checksum(vbody) + "," + vbody
        else:
            w.visible = w.layout

    def flags(self, w):
        f = "*" if w.id == self.active_window else "-"
        return f + ("Z" if w.zoom is not None else "")

    def layout_change(self, w):
        self.relayout(w)
        emit(("%%layout-change @%d %s %s %s" % (w.id, w.layout, w.visible, self.flags(w))).encode())

    # ── stream ───────────────────────────────────────────────────────────────────────────────
    def output(self, pid, data: bytes, cut=None):
        """%output for pane `pid`; `cut` = list of byte offsets where tmux would split the data."""
        if isinstance(data, str):
            data = data.encode()
        pieces = []
        last = 0
        for c in (cut or []):
            pieces.append(data[last:c])
            last = c
        pieces.append(data[last:])
        for piece in pieces:
            if self.panes.get(pid) and self.panes[pid].paused:
                return
            if self.extended:
                emit(b"%%extended-output %%%d 0 : " % pid + encode_output(piece))
            else:
                emit(b"%%output %%%d " % pid + encode_output(piece))

    def notify(self, line: str):
        emit(line.encode())

    def on(self, pattern, fn, before=False, once=True):
        """React to a received command line matching `pattern` (re.search): fn(srv, line) runs
        after the line's blocks (or before them with before=True)."""
        self.reactions.append([re.compile(pattern), fn, before, once])

    def react(self, line, before):
        for r in list(self.reactions):
            if r[2] == before and r[0].search(line):
                if r[3]:
                    self.reactions.remove(r)
                r[1](self, line)

    # ── formats ──────────────────────────────────────────────────────────────────────────────
    def pane_fields(self, p):
        w = self.window_of(p.id)
        x, y = p.cursor
        f = {
            "pane_id": "%%%d" % p.id, "window_id": "@%d" % (w.id if w else 0),
            "pane_active": "1", "pane_width": str(p.w), "pane_height": str(p.h),
            "pane_current_path": p.path, "history_size": str(len(p.history)),
            "cursor_x": str(x), "cursor_y": str(y), "cursor_flag": "1",
            "alternate_on": "1" if p.alt else "0",
            "alternate_saved_x": "0" if p.alt else "4294967295",
            "alternate_saved_y": "0" if p.alt else "4294967295",
            "scroll_region_upper": "0", "scroll_region_lower": str(p.h - 1),
            "keypad_cursor_flag": "0", "keypad_flag": "0", "insert_flag": "0", "wrap_flag": "1",
            "origin_flag": "0", "mouse_standard_flag": "0", "mouse_button_flag": "0",
            "mouse_any_flag": "0", "mouse_sgr_flag": "0", "mouse_utf8_flag": "0",
            "bracket_paste_flag": "0", "cursor_shape": "default", "cursor_blinking": "0",
            "pane_in_mode": "0", "pane_title": p.title, "pane_pid": str(4000 + p.id),
        }
        f.update(p.modes)
        return f

    def window_fields(self, w):
        return {
            "window_id": "@%d" % w.id, "window_index": str(w.index),
            "window_active": "1" if w.id == self.active_window else "0",
            "window_raw_flags": self.flags(w), "window_layout": w.layout,
            "window_visible_layout": w.visible, "window_name": w.name,
        }

    @staticmethod
    def expand(fmt, fields):
        return re.sub(r"#\{([a-z_]+)\}", lambda m: fields.get(m.group(1), ""), fmt)

    # ── commands ─────────────────────────────────────────────────────────────────────────────
    @staticmethod
    def split_line(line):
        """tmux's lexer, enough for SolidTerm's lines: quotes (single literal, double with \\),
        adjacent quoted pieces join, ' ; ' separates commands."""
        cmds, args, cur, have = [], [], "", False
        i, n = 0, len(line)
        while i < n:
            ch = line[i]
            if ch == "'":
                j = line.index("'", i + 1)
                cur += line[i + 1:j]
                have = True
                i = j + 1
                continue
            if ch == '"':
                i += 1
                while line[i] != '"':
                    if line[i] == "\\":
                        i += 1
                    cur += line[i]
                    i += 1
                have = True
                i += 1
                continue
            if ch == " ":
                if have:
                    args.append(cur)
                cur, have = "", False
                i += 1
                continue
            if ch == ";" and not have:
                if args:
                    cmds.append(args)
                args = []
                i += 1
                continue
            cur += ch
            have = True
            i += 1
        if have:
            args.append(cur)
        if args:
            cmds.append(args)
        return cmds

    def block(self, lines, error=False):
        self.number += 1
        t = int(time.time())
        emit(b"%%begin %d %d 1" % (t, self.number))
        for l in lines:
            emit(l if isinstance(l, bytes) else l.encode())
        emit(b"%%%s %d %d 1" % (b"error" if error else b"end", t, self.number))

    @staticmethod
    def opts(args, valued):
        """args → (flags set, {flag: value}, positionals)."""
        flags, vals, pos = set(), {}, []
        i = 0
        while i < len(args):
            a = args[i]
            if a.startswith("-") and len(a) > 1 and not a[1].isdigit():
                for k, ch in enumerate(a[1:]):
                    if ch in valued:
                        rest = a[2 + k:]
                        if rest:
                            vals[ch] = rest
                        else:
                            i += 1
                            vals[ch] = args[i]
                        break
                    flags.add(ch)
            else:
                pos.append(a)
            i += 1
        return flags, vals, pos

    def target_pane(self, vals):
        t = vals.get("t")
        if t and t.startswith("%"):
            return self.panes.get(int(t[1:]))
        w = self.find_window(self.active_window)
        return self.panes.get(self.leaves(w.tree)[0]) if w else None

    def run(self, args):
        """One command → (lines, error)."""
        name = args[0]
        if name == "refresh-client":
            flags, vals, pos = self.opts(args[1:], "CfAB")
            if "C" in vals:
                m = re.match(r"(\d+)x(\d+)$", vals["C"])
                if m:
                    self.cols, self.rows = int(m.group(1)), int(m.group(2))
                    self.after_block.append(lambda: [self.layout_change(w) for w in self.windows])
            if "f" in vals and vals["f"].startswith("pause-after"):
                self.extended = True
            if "A" in vals:
                pid, _, what = vals["A"].partition(":")
                p = self.panes.get(int(pid[1:]))
                if p and what == "continue":
                    p.paused = False
                    self.after_block.append(lambda pid=p.id: emit(b"%%continue %%%d" % pid))
                elif p and what == "pause":
                    p.paused = True
                    self.after_block.append(lambda pid=p.id: emit(b"%%pause %%%d" % pid))
            return [], False
        if name == "list-windows":
            flags, vals, pos = self.opts(args[1:], "Ft")
            fmt = vals.get("F", "#{window_index}: #{window_name}")
            return [self.expand(fmt, self.window_fields(w)) for w in self.windows], False
        if name == "list-panes":
            flags, vals, pos = self.opts(args[1:], "Ft")
            fmt = vals.get("F", "#{pane_id}")
            out = []
            for w in self.windows:
                for pid in self.leaves(w.tree):
                    out.append(self.expand(fmt, self.pane_fields(self.panes[pid])))
            return out, False
        if name == "display-message":
            flags, vals, pos = self.opts(args[1:], "tFc")
            p = self.target_pane(vals)
            if vals.get("t", "").startswith("%") and p is None:
                return ["can't find pane: " + vals["t"]], True
            fmt = pos[0] if pos else ""
            return [self.expand(fmt, self.pane_fields(p) if p else {})], False
        if name == "capture-pane":
            flags, vals, pos = self.opts(args[1:], "tSEb")
            p = self.target_pane(vals)
            if p is None:
                return ["can't find pane: " + vals.get("t", "")], True
            if "P" in flags:
                return [encode_capture(p.pending)], False
            if "a" in flags:
                rows = (list(p.saved) + [b""] * max(0, p.h - len(p.saved))) if p.alt else []
                if not p.alt and "q" not in flags:
                    return ["no alternate screen"], True
            else:
                screen = list(p.screen) + [b""] * max(0, p.h - len(p.screen))
                s, e = vals.get("S"), vals.get("E")
                hist = []
                if s is not None and s != "-" and int(s) < 0:
                    hist = p.history[int(s):] if -int(s) <= len(p.history) else list(p.history)
                elif s == "-":
                    hist = list(p.history)
                rows = hist + ([] if e == "-1" else screen)
            return [encode_capture(r) for r in rows], False
        if name == "detach-client":
            self.exiting = True
            return [], False
        if name == "kill-pane":
            return ["can't find pane: " + args[-1]], True
        return ["parse error: unknown command: " + name], True

    def handle(self, line):
        if self.log_path:
            with open(self.log_path, "a") as f:
                f.write(line + "\n")
        if line == "":
            self.exiting = True
            return
        self.react(line, True)
        try:
            cmds = self.split_line(line)
        except (ValueError, IndexError):
            self.block(["parse error: syntax error"], True)
            return
        self.after_block = []
        if any(c[0] == "capture-pane" for c in cmds):
            self.capture_groups += 1
        for c in cmds:
            lines, err = self.run(c)
            self.block(lines, err)
            if err:
                break  # cmdq_remove_group: the rest of the line never runs
        for fn in self.after_block:
            fn()
        self.react(line, False)
        if self.exiting:
            emit(b"%exit")
            sys.exit(0)

    def serve(self):
        t = int(time.time())
        emit(b"%%begin %d 1 0" % t)
        emit(b"%%end %d 1 0" % t)
        emit(("%%session-changed $%d %s" % (self.session, self.session_name)).encode())
        for raw in sys.stdin.buffer:
            self.handle(raw.rstrip(b"\n").decode("utf-8", "replace"))
        # stdin closed: tmux drops the client
        sys.exit(0)


def main():
    srv = Server()
    scenario = os.environ.get("FAKE_TMUX_SCENARIO")
    if scenario:
        runpy.run_path(scenario)["setup"](srv)
    srv.serve()


if __name__ == "__main__":
    main()
