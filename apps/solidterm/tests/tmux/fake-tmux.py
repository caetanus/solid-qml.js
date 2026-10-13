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
        self.exiting_reason = ""
        self.fixed = {}            # pane id -> ("x"|"y", cells) from resize-pane
        self.active_pane = {}      # window id -> pane id (default: its first pane)
        self.capture_groups = 0
        self.after_block = []

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
        axis = "x" if kind == "{" else "y"
        total = (w if kind == "{" else h) - (n - 1)
        # resize-pane -x/-y pins a child's size (a leaf, or a split of the other kind holding it).
        sizes = [None] * n
        for i, c in enumerate(children):
            if isinstance(c, int) or c[0] != kind:
                for pid in self.leaves(c):
                    f = self.fixed.get(pid)
                    if f and f[0] == axis:
                        sizes[i] = max(1, min(f[1], total - (n - 1)))
        free = total - sum(v for v in sizes if v is not None)
        loose = [i for i in range(n) if sizes[i] is None]
        for k, i in enumerate(loose):
            sizes[i] = free // len(loose) if k < len(loose) - 1 else free - (free // len(loose)) * (len(loose) - 1)
        if not loose:
            sizes[-1] += total - sum(sizes)
        pos = x if kind == "{" else y
        for i, c in enumerate(children):
            if kind == "{":
                parts.append(self.cell(c, pos, y, sizes[i], h))
            else:
                parts.append(self.cell(c, x, pos, w, sizes[i]))
            pos += sizes[i] + 1
        if kind == "{":
            return "%dx%d,%d,%d{%s}" % (w, h, x, y, ",".join(parts))
        return "%dx%d,%d,%d[%s]" % (w, h, x, y, ",".join(parts))

    # ── tree edits (split, move, break, join, kill) ──────────────────────────────────────────
    def remove_leaf(self, tree, pid):
        if isinstance(tree, int):
            return None if tree == pid else tree
        kind, children = tree
        kids = [k for k in (self.remove_leaf(c, pid) for c in children) if k is not None]
        if not kids:
            return None
        if len(kids) == 1:
            return kids[0]
        # a child of the same kind merges into us (tmux never nests a split in one of its kind)
        flat = []
        for k in kids:
            flat += k[1] if not isinstance(k, int) and k[0] == kind else [k]
        return (kind, flat)

    def insert_beside(self, tree, target, pid, kind, before):
        if isinstance(tree, int):
            if tree != target:
                return tree
            return (kind, [pid, tree] if before else [tree, pid])
        k, children = tree
        out = []
        for c in children:
            if c == target and k == kind:
                out += [pid, c] if before else [c, pid]
            else:
                out.append(self.insert_beside(c, target, pid, kind, before))
        return (k, out)

    def new_pane_id(self):
        return max(self.panes.keys() | {0}) + 1

    def new_window_id(self):
        return max([w.id for w in self.windows] + [0]) + 1

    def take_pane(self, pid):
        """Remove pane pid from its window; a window left empty goes (3.7c: %unlinked-window-close).
        Returns the window it was in."""
        w = self.window_of(pid)
        if not w:
            return None
        w.tree = self.remove_leaf(w.tree, pid)
        if w.zoom == pid:
            w.zoom = None
        if w.tree is None:
            self.windows.remove(w)
            self.after_block.append(lambda w=w: emit(b"%%unlinked-window-close @%d" % w.id))
        else:
            self.after_block.append(lambda w=w: self.layout_change(w))
        return w

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
    def output(self, pid, data: bytes, cut=None, gap=0.0):
        """%output for pane `pid`; `cut` = list of byte offsets where tmux would split the data;
        `gap` seconds between the pieces (so they reach the pane's parser in separate reads)."""
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
            if gap:
                time.sleep(gap)

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
            "pane_active": "1" if w and self.active_pane.get(w.id, self.leaves(w.tree)[0]) == p.id else "0",
            "pane_width": str(p.w), "pane_height": str(p.h),
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
            "pane_in_mode": "0", "pane_mode": "", "pane_title": p.title, "pane_pid": str(4000 + p.id),
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
        if name in ("split-window", "splitw"):
            flags, vals, pos = self.opts(args[1:], "tcl")
            p = self.target_pane(vals)
            if p is None:
                return ["can't find pane: " + vals.get("t", "")], True
            w = self.window_of(p.id)
            nid = self.new_pane_id()
            self.pane(nid, screen=[b"$ "], cursor=(2, 0), title="new %d" % nid)
            w.tree = self.insert_beside(w.tree, p.id, nid, "{" if "h" in flags else "[", "b" in flags)
            self.active_pane[w.id] = nid
            self.after_block.append(lambda: self.layout_change(w))
            self.after_block.append(lambda: emit(b"%%window-pane-changed @%d %%%d" % (w.id, nid)))
            return [], False
        if name in ("new-window", "neww"):
            flags, vals, pos = self.opts(args[1:], "tcn")
            nid, wid = self.new_pane_id(), self.new_window_id()
            self.pane(nid, screen=[b"$ "], cursor=(2, 0), title="")
            nw = self.window(wid, vals.get("n", "new"), nid)
            if "d" not in flags:
                self.active_window = wid
                self.after_block.append(lambda: emit(b"%%session-window-changed $%d @%d" % (self.session, wid)))
            self.after_block.append(lambda: emit(b"%%window-add @%d" % wid))
            return [], False
        if name in ("select-window", "selectw"):
            flags, vals, pos = self.opts(args[1:], "t")
            w = self.find_window(int(vals.get("t", "@-1")[1:]))
            if w is None:
                return ["can't find window: " + vals.get("t", "")], True
            self.active_window = w.id
            self.after_block.append(lambda: emit(b"%%session-window-changed $%d @%d" % (self.session, w.id)))
            return [], False
        if name in ("resize-pane", "resizep"):
            flags, vals, pos = self.opts(args[1:], "txy")
            p = self.target_pane(vals)
            if p is None:
                return ["can't find pane: " + vals.get("t", "")], True
            w = self.window_of(p.id)
            if "Z" in flags:
                w.zoom = None if w.zoom == p.id else p.id
            if "x" in vals:
                self.fixed[p.id] = ("x", int(vals["x"]))
            if "y" in vals:
                self.fixed[p.id] = ("y", int(vals["y"]))
            self.after_block.append(lambda: self.layout_change(w))
            return [], False
        if name in ("swap-pane", "swapp"):
            flags, vals, pos = self.opts(args[1:], "st")
            a, b = int(vals["s"][1:]), int(vals["t"][1:])
            wa, wb = self.window_of(a), self.window_of(b)
            def swap(t):
                if isinstance(t, int):
                    return b if t == a else a if t == b else t
                return (t[0], [swap(c) for c in t[1]])
            for w in {wa, wb}:
                w.tree = swap(w.tree)
                self.after_block.append(lambda w=w: self.layout_change(w))
            return [], False
        if name in ("move-pane", "movep", "join-pane", "joinp"):
            flags, vals, pos = self.opts(args[1:], "stl")
            a = int(vals["s"][1:])
            t = vals["t"]
            if self.window_of(a) is None:
                return ["can't find pane: " + vals["s"]], True
            if t.startswith("@"):
                tw = self.find_window(int(t[1:]))
                if tw is None:
                    return ["can't find window: " + t], True
                b = self.active_pane.get(tw.id)
                if b not in self.leaves(tw.tree) or b == a:
                    b = [x for x in self.leaves(tw.tree) if x != a][0]
            else:
                b = int(t[1:])
                tw = self.window_of(b)
            self.take_pane(a)
            tw.tree = self.insert_beside(tw.tree, b, a, "{" if "h" in flags else "[", "b" in flags)
            # 3.7c's order (recorded): the source window's close/layout first, then the target's
            self.after_block.append(lambda: emit(b"%%window-pane-changed @%d %%%d" % (tw.id, a)))
            self.after_block.append(lambda: self.layout_change(tw))
            return [], False
        if name in ("break-pane", "breakp"):
            flags, vals, pos = self.opts(args[1:], "stn")
            a = int(vals["s"][1:])
            if self.window_of(a) is None:
                return ["can't find pane: " + vals["s"]], True
            self.take_pane(a)
            wid = self.new_window_id()
            self.window(wid, "broken", a)
            if "d" not in flags:
                self.active_window = wid
                self.after_block.append(lambda: emit(b"%%session-window-changed $%d @%d" % (self.session, wid)))
            self.after_block.append(lambda: emit(b"%%window-add @%d" % wid))
            return [], False
        if name in ("kill-pane", "killp"):
            flags, vals, pos = self.opts(args[1:], "t")
            p = self.target_pane(vals)
            if p is None or not vals.get("t", "").startswith("%"):
                return ["can't find pane: " + vals.get("t", "")], True
            self.take_pane(p.id)
            self.panes.pop(p.id, None)
            return [], False
        if name in ("kill-window", "killw"):
            flags, vals, pos = self.opts(args[1:], "t")
            w = self.find_window(int(vals.get("t", "@-1")[1:]))
            if w is None:
                return ["can't find window: " + vals.get("t", "")], True
            self.windows.remove(w)
            self.after_block.append(lambda: emit(b"%%unlinked-window-close @%d" % w.id))
            return [], False
        if name in ("rename-window", "renamew"):
            flags, vals, pos = self.opts(args[1:], "t")
            w = self.find_window(int(vals.get("t", "@-1")[1:]))
            if w is None:
                return ["can't find window"], True
            w.name = pos[0] if pos else ""
            self.after_block.append(lambda: emit(("%%window-renamed @%d %s" % (w.id, w.name)).encode()))
            return [], False
        if name == "select-pane":
            flags, vals, pos = self.opts(args[1:], "tT")
            t = vals.get("t", "")
            if not t.startswith("%") or self.panes.get(int(t[1:])) is None:
                return ["can't find pane: " + t], True
            w = self.window_of(int(t[1:]))
            if w:
                self.active_pane[w.id] = int(t[1:])
                self.after_block.append(lambda w=w, t=t: emit(("%%window-pane-changed @%d %s" % (w.id, t)).encode()))
            return [], False
        if name == "send-keys":
            flags, vals, pos = self.opts(args[1:], "tNc")
            if vals.get("t", "").startswith("%") and self.panes.get(int(vals["t"][1:])) is None:
                return ["can't find pane: " + vals["t"]], True
            p = self.target_pane(vals)
            if "X" in flags and p and p.modes.get("pane_in_mode") == "1" and pos[-1:] == ["cancel"]:
                p.modes["pane_in_mode"], p.modes["pane_mode"] = "0", ""
                self.after_block.append(lambda: emit(b"%%pane-mode-changed %%%d" % p.id))
            return [], False
        if name in ("set-option", "set"):
            return [], False
        if name in ("kill-session", "kill-ses"):
            self.exiting_reason = "server exited"
            self.exiting = True
            return [], False
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
            emit(b"%exit" + ((" " + self.exiting_reason).encode() if self.exiting_reason else b""))
            sys.exit(0)

    def serve(self):
        if self.log_path:
            with open(self.log_path, "a") as f:
                f.write("# argv: " + " ".join(sys.argv[1:]) + "\n")
        t = int(time.time())
        emit(b"%%begin %d 1 0" % t)
        emit(b"%%end %d 1 0" % t)
        emit(("%%session-changed $%d %s" % (self.session, self.session_name)).encode())
        for raw in sys.stdin.buffer:
            self.handle(raw.rstrip(b"\n").decode("utf-8", "replace"))
        # stdin closed: tmux drops the client
        sys.exit(0)


def sessions_env():
    """FAKE_TMUX_SESSIONS="name:ours:attached;…" — the sessions one-shot commands see."""
    out = []
    for item in filter(None, os.environ.get("FAKE_TMUX_SESSIONS", "").split(";")):
        name, ours, attached = (item.split(":") + ["", "", "0"])[:3]
        out.append((name, ours, attached))
    return out


def one_shot(argv):
    """tmux without -C: -V, has-session, list-sessions (what SolidTerm runs before attaching and
    for its session picker). Logged like the control commands."""
    log = os.environ.get("FAKE_TMUX_LOG")
    if log:
        with open(log, "a") as f:
            f.write("# oneshot: " + " ".join(argv) + "\n")
    if argv[:1] == ["-V"]:
        print(os.environ.get("FAKE_TMUX_VERSION", "tmux 3.7c"))
        return 0
    if argv[:1] == ["has-session"]:
        target = argv[argv.index("-t") + 1].lstrip("=") if "-t" in argv else ""
        return 0 if any(n == target for n, _, _ in sessions_env()) else 1
    if argv[:1] == ["list-sessions"]:
        fmt = argv[argv.index("-F") + 1] if "-F" in argv else "#{session_name}"
        for n, ours, attached in sessions_env():
            f = {"session_name": n, "@solidterm": ours, "session_attached": attached}
            print(re.sub(r"#\{([@a-z_]+)\}", lambda m: f.get(m.group(1), ""), fmt))
        return 0
    return 1


def main():
    if "-C" not in sys.argv[1:]:
        sys.exit(one_shot(sys.argv[1:]))
    srv = Server()
    scenario = os.environ.get("FAKE_TMUX_SCENARIO")
    if scenario:
        runpy.run_path(scenario)["setup"](srv)
    srv.serve()


if __name__ == "__main__":
    main()
