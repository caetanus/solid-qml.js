# SolidTerm × tmux control mode (design)

Status: **phases 0, 1 and 2 implemented** (viewer with input); see "Implementation status" at the
end for what real tmux 3.7c corrected. Owner decision
(2026-10-13): SolidTerm integrates with tmux **control mode** the way iTerm2 does with `tmux -CC`.
tmux windows become native SolidTerm tabs, tmux panes become native splits, and each pane is a
native `TerminalView` fed by tmux's `%output`. Scrolling, selection, search, links, mouse
reporting and pause-on-select stay native. Persistence comes from tmux: sessions survive SolidTerm
restarts, and later this can run against a remote host over ssh.

Why: inside GNU screen (where the owner's agents live today) the wheel and selection operate on
screen's redrawn picture, not on the program's output, so native scrollback and selection do not
work. Control mode gives us the program's **raw** output per pane (section 1.4), so our own
`TermScreen` owns the history again.

Reference: tmux **3.7c** source (tag `3.7c`, commit `e476c12`), cloned read-only for this design
and deleted afterwards. Every protocol claim below cites `file:line` in that tree. Behaviour
expectations also come from the iTerm2 tmux integration page and the tmux wiki "Control Mode"
page. No tmux code is copied: we write our own client.

---

## 0. Vocabulary

| tmux | id syntax | SolidTerm |
|---|---|---|
| server | socket (`$TMUX_TMPDIR/tmux-UID/default`) | one `TmuxServer` handle per socket (local, later per ssh host) |
| session | `$N` | one **SolidTerm OS window** (one control client) |
| window | `@N` | one **tab** (`TerminalTabs::Tab`) |
| pane | `%N` | one **split leaf** (`TerminalPanes::Node` with a `TerminalView`) |
| layout string | `b25f,80x24,0,0{…}` | the split tree of that tab |

Ids, not names or indexes, identify everything: they are unique for the life of the server, and the
tmux wiki advises clients to use them.

---

## 1. Protocol

### 1.1 Spawning the control client: `-C` over pipes, not `-CC` over a pty

The tmux *client* process is only a relay. On start it sends dup()s of its stdin and stdout to the
server (`client.c:477-482`, `MSG_IDENTIFY_STDIN/STDOUT`), and the **server** reads commands from
and writes notifications to those fds directly (`server-client.c:2552-2553` → `control_start`,
`control.c:770-808`). So what we choose for stdin/stdout is what the server talks over.

| | `tmux -C` (one C) | `tmux -CC` (two) |
|---|---|---|
| flag | `CLIENT_CONTROL` (`tmux.c:418-423`) | `CLIENT_CONTROL\|CLIENT_CONTROLCONTROL` |
| stdin must be a tty | no (`ttyname` may fail, `client.c:306-307`; the client is then named `client-PID`, `server-client.c:2539-2542`) | **yes**: `tcgetattr` on stdin or exit 1 (`client.c:343-361`), then raw mode with `ONLCR` (output lines end `\r\n`) |
| framing | none | `\eP1000p` on entry (`control.c:804-807`), `%exit` then `\e\\` on exit (`client.c:438-441`) |
| fds | separate in/out (`control.c:795-800`) | one fd both ways (`control.c:775-777, 793-794`) |

**Decision: `tmux -C` with stdin/stdout on pipes** (`pipe2(O_CLOEXEC)` + `posix_spawn`, or a
`socketpair` for stdin), stderr on a third pipe for diagnostics. Reasons:

- no line discipline: no echo, no `\r\n` translation, and no canonical-mode line limit (`MAX_CANON`,
  4096 on Linux), which would truncate a long `send-keys -H` command. The wiki's warning that single
  `-C` "leaves the terminal in canonical mode" is about running it on a tty, which we do not;
- no DCS framing to strip;
- a dedicated reader thread can `poll(2)` the stdout pipe exactly as `TermWorker` polls a pty master.

Command line, local server:

```
tmux -C new-session -A -s <name> -x <cols> -y <rows> [-c <cwd>]   # attach, or create then attach
tmux -C attach-session -t <name>                                  # attach only (fail if missing)
tmux -C attach-session -r -t <name>                               # read-only (client flag, server-client.c:2657)
```

`-x/-y` on `new-session` work for control clients (CHANGES 2.6, line 1974), so a new session starts
at our size instead of `default-size` (80x24).

Environment of the spawned client: **unset `TMUX`** (otherwise `new`/`attach` refuse with "sessions
should be nested with care"), keep `TMUX_TMPDIR`. If the server is not running, `new-session`
starts it and it inherits *our* environment (SolidTerm's), which every pane then inherits. That
matches today's behaviour (a local pane inherits SolidTerm's env).

**`-CC` / DCS `\eP1000p` is still needed for one case:** the user runs `tmux -CC` *by hand inside an
ordinary SolidTerm pane* (typically after `ssh host`). Then the control stream arrives inside that
pane's pty output, wrapped as `\eP1000p … \e\\` with `\r\n` line ends. Supporting it means a
"gateway" mode in the pane: on seeing `\eP1000p`, stop feeding libvterm and feed the bytes to a
`TmuxClient` until `\e\\`, writing commands back into the pty. That is phase 6; for remote hosts
the simpler route is to spawn `ssh -T host tmux -C attach -t S` with pipes, which needs no DCS at
all.

### 1.2 Lines and command blocks

Everything tmux sends is newline-terminated lines (`control_vwrite` appends `\n`,
`control.c:403-404`). Commands we send are one per line; the server reads with
`evbuffer_readln(EOL_LF)` and parses each line with the normal command parser
(`control.c:562-580`). **An empty line detaches the client** (`control.c:567-571`).

Each command produces exactly one block (`tmux.1:8121-8144`):

```
%begin <time> <number> <flags>
…output lines (possibly none)…
%end <time> <number> <flags>          (or %error <time> <number> <flags>)
```

The guard lines are written by `cmdq_guard` (`cmd-queue.c:825-833`). Facts that shape our
correlator:

- `<number>` comes from a **server-global** counter assigned when the command fires
  (`cmd-queue.c:737, 770`). We cannot predict it; it only pairs `%begin` with its `%end`.
- `<flags>` is 1 only when the command came from **this control client's input**
  (`cmd-queue.c:618`: `!!(state->flags & CMDQ_STATE_CONTROL)`, set in `control.c:573`). The
  command given on our argv (`new-session`/`attach-session`), hooks and config-file commands
  produce blocks with flags 0. **We correlate only flags-1 blocks**, strictly FIFO against our
  queue of sent commands; flags-0 blocks are logged and dropped.
- A parse error still produces one block: `%begin`, `parse error: …`, `%error`
  (`control.c:527-539`).
- Several commands on one line (`a ; b`) share a **group** (`cmd-queue.c:516`). If one fails, the
  rest of its group is removed without running (`cmd-queue.c:776-781`, `cmdq_remove_group`
  `470-483`): **no blocks** for them. So the pending-command queue records group boundaries, and
  on `%error` it discards the remaining entries of that group.
- "A notification will never occur inside an output block" (`tmux.1:8154`), and a command's guard
  and output lines are written in one go while the command runs (no pane read can interleave:
  pane output is queued only from `window_pane_read_callback`, `window.c:1139-1164`, an event
  callback that cannot run inside `cmdq_next`).
- Output lines inside a block are raw text. A captured pane line could in theory read
  `%end 1700000000 42 1`; we only accept the guard whose number equals the open `%begin`'s, and we
  capture with `-C` (escapes non-printables, section 4) so the risk is a printable line that
  exactly forges the current number.

### 1.3 Notifications (complete list for 3.7c)

From `control-notify.c` plus the other `control_write` sites. Lines start with `%`; arguments
are space-separated; a trailing name may contain spaces (it is "the rest of the line").

| notification | emitted at | our action |
|---|---|---|
| `%output %P <data>` | `control.c:631` | decode (1.4), route to pane P |
| `%extended-output %P <age-ms> … : <data>` | `control.c:627-629` (when `pause-after` is set) | same; ignore args between age and ` : ` (`tmux.1:8181-8192`) |
| `%pause %P` / `%continue %P` | `control.c:389, 375, 461` | flow control (1.6) |
| `%layout-change @W <layout> <visible-layout> <raw-flags>` | `control-notify.c:52-71` | rebuild tab W's split tree (section 2) |
| `%window-add @W` / `%window-close @W` | `control-notify.c:121, 103` | add/remove tab |
| `%window-renamed @W <name>` | `control-notify.c:139-140` | tab name |
| `%unlinked-window-add/close/renamed @W …` | `control-notify.c:123, 105, 142-143` | window not in our session (session groups, other sessions): ignore except for a session picker |
| `%window-pane-changed @W %P` | `control-notify.c:86-87` | active pane of W → our focused leaf (2.4) |
| `%session-window-changed $S @W` | `control-notify.c:231-232` | active window → active tab, if `$S` is ours |
| `%session-changed $S <name>` | `control-notify.c:163-164` | we are (now) attached to S: (re)build the whole model |
| `%session-renamed $S <name>` | `control-notify.c:192` | OS window title. Note: the man page says `%session-renamed name` (`tmux.1:8237`); the code also sends the id. Parse both shapes |
| `%sessions-changed` | `control-notify.c:205, 218` | refresh the session picker if open |
| `%client-session-changed <client> $S <name>` | `control-notify.c:166-167` | another client switched session: informational |
| `%client-detached <client>` | `control-notify.c:179` | informational (another client left) |
| `%pane-mode-changed %P` | `control-notify.c:39` | a pane entered/left a tmux mode (copy mode etc., 6.6) |
| `%paste-buffer-changed <name>` / `%paste-buffer-deleted <name>` | `control-notify.c:245, 258` | optional clipboard sync |
| `%subscription-changed <name> $S @W <idx> %P … : <value>` | `control.c:871-1000` | format subscriptions (1.7) |
| `%message <text>` | `cmd-display-message.c:151` | show as a transient notice |
| `%config-error <error>` | `cfg.c:229, 253` (since 3.4, CHANGES:789) | show once per attach |
| `%exit [reason]` | `client.c:423-428` (printed by the client process) | the control client is ending; see 5 and 7 |

Exit reasons (`client.c:184-215`): `detached`, `detached (from session S)`, `detached and SIGHUP`,
`lost tty`, `terminated`, `server exited unexpectedly`, `exited`, `server exited`, plus
`too far behind` set by the server (`control.c:465`).

### 1.4 `%output`: the bytes are the program's raw output

`window_pane_read_callback` (`window.c:1139-1164`) first queues the newly read pty bytes for every
control client (`control_write_output`, line 1160), **then** runs tmux's own parser on them
(`input_parse_pane`, line 1162). The bytes sent are `window_pane_get_new_data` of the pane's input
buffer (`control.c:634`): exactly what the program wrote to its pty, not a tmux redraw. So our
`TermScreen`/libvterm interprets the program directly, as with a local pty. This is the property
that gives back native scrollback and selection.

Encoding (`control.c:637-648`): every byte `< 0x20` and the backslash are written as `\ooo` (three
octal digits); every other byte, including `0x7f` and all of `0x80-0xff`, is written as is. The
decoder is a single pass: `\` followed by three octal digits → one byte, else copy. Splits are
arbitrary: one pane's data is cut wherever the per-write budget ends (`control.c:705-708`, fair
share `space / pending_count / 3` in `control.c:751`), so one `%output` line can end inside a UTF-8
sequence or an escape sequence. Our parser is a stream parser, so that is harmless.

Ordering (`control.c:29-43`): every client has one queue of blocks; an `%output` block holds back
all later non-output lines until it is fully written. **Notifications and command replies are
therefore totally ordered with pane output.** Section 4 relies on this.

### 1.5 Issuing commands

One `TmuxClient` per control connection owns:

- `std::deque<Pending>` of sent commands: `{ kind, group id, last-in-group, target ids, callback }`.
  The callback is a plain function pointer + `void *` (owner rule: no `std::function`).
- a writer on the GUI thread (commands are small; written non-blocking, the rest queued and flushed
  on `POLLOUT` by the reader thread, the same pattern as `TermWorker::queueOutput`,
  `termworker.h:98-100`).

Argument quoting: single quotes make everything literal; `\`, `~` and `$` are only expanded outside
single quotes (`cmd-parse.y:1700-1716`). We wrap every user-provided string (names, paths, `-l`
text) in single quotes and send a `'` itself as `send-keys -H 27`. A line can never contain `\n`.

### 1.6 Flow control

What tmux does by default, without flags:

- A control client whose write buffer holds ≥ `CONTROL_BUFFER_LOW` (512) bytes marks its panes
  "off" for reading (`control.c:333`); if **every** attached client is a control client unable to
  take more, tmux stops reading the pane's pty (`server-client.c:1684-1745`), so the program blocks
  in `write(2)`. A normal (tty) client attached to the same session keeps the pane reading
  (`server-client.c:1684-1687`).
- Output older than `CONTROL_MAXIMUM_AGE` = 300 000 ms that is still unsent **disconnects the
  whole client** with `too far behind` (`control.c:137-138, 462-467`).

With `refresh-client -f pause-after=N` (seconds; stored ×1000 as ms, `server-client.c:2628-2630`;
available since 3.2, CHANGES:1111-1114):

- output is sent as `%extended-output` with its age (`control.c:626-629`);
- a pane whose oldest unsent output is older than N s is **paused**: its queued data is discarded
  and `%pause %P` is sent (`control.c:456-461`). tmux then stops sending that pane's data
  (`control.c:325-327, 491-492`);
- `refresh-client -A '%P:continue'` resumes: the offsets jump to the pane's current position (the
  skipped output is lost) and `%continue %P` is sent (`control.c:366-376`).
- `-A '%P:pause'` pauses on request; `-A '%P:off'` / `'%P:on'` stop/restart sending a pane
  (`cmd-refresh-client.c:134-164`, `control.c:338-362`; `tmux.1:1488-1514`).

**Our policy:** set `pause-after=5` right after attaching. A stalled SolidTerm (debugger, suspended
window) then costs one reseed per affected pane (section 4.4) instead of a disconnect after five
minutes. The reader thread never stops reading the control pipe for one pane's sake (that would
stall every pane); per-pane holds are done on our side (section 6.1).

### 1.7 Subscriptions (optional, at most 1 Hz)

`refresh-client -B name:what:format` (`cmd-refresh-client.c:47-79`, `control.c:1137-1164`): `what`
is empty (session), `%P`, `%*`, `@W` or `@*`; changes arrive as `%subscription-changed` at most
once a second (`control.c:1051, 1056`). Useful for the pane header (`#{pane_current_command}`,
`#{pane_current_path}`) without polling. Not needed for correctness.

### 1.8 Colours reported to programs

tmux answers terminal queries itself (DA, DSR 6n, XTVERSION, DECRQM…: `input.c:1563-1578,
1696-1706, 1863`). For OSC 10/11 (foreground/background colour queries) it uses colours a control
client provides with `refresh-client -r '%P:<OSC 10/11 reply>'` (since 3.5, CHANGES:648;
`cmd-refresh-client.c:167-192`, used at `window.c:1958-2004`). We send our theme's colours for every
pane on attach and on a theme change, so programs that detect a light or dark background (vim,
delta, bat) see SolidTerm's colours.

---

## 2. Model mapping

### 2.1 Objects

```
TmuxServer (socket path)             ── one per server we talk to
  └─ TmuxClient (one `tmux -C` process, one session)  ── owns the pipes, reader thread, command queue
       └─ session $S  ⇄  one SolidTerm QQuickWindow   (TerminalTabs in tmux mode)
            └─ window @W ⇄ Tab { panes: TerminalPanes (tmux mode), tmuxWindowId }
                 └─ pane %P ⇄ Node leaf { view: TerminalView (source = tmux pane) }
```

A SolidTerm window is either **all tmux** (one session) or **all local**; mixing is refused, as in
iTerm2 ("a tab with a tmux window may not contain non-tmux split panes"). Two SolidTerm windows may
show two sessions of the same server (two `TmuxClient`s). Showing one tmux window in a *second*
OS window of the same session is phase 6 (needs per-window sizes, 3.4).

### 2.2 Layout string → split tree

Grammar (`layout-custom.c:91-134` writes it, `322-425` reads it):

```
layout   := csum "," cell [ "<" cell { "," cell } ">" ]       ; "<…>" = floating panes (3.7, 60-87)
csum     := 4 lowercase hex digits                            ; "%04hx"
cell     := W "x" H "," X "," Y ( "," paneid                  ; leaf: pane id WITHOUT the '%'
                              | "{" cell { "," cell } "}"      ; left-right  = side by side
                              | "[" cell { "," cell } "]" )    ; top-bottom  = stacked
```

All numbers are cells. Children of a `{}` node have the parent's height and widths that sum to the
parent's width **minus one column per border** (`layout_check`, `layout-custom.c:137-170`); `[]`
likewise with rows. Checksum (`layout-custom.c:46-57`), over the text after the comma:

```
csum = 0; for each byte b: csum = (csum >> 1) + ((csum & 1) << 15); csum += b;   (16-bit)
```

We verify it on every `%layout-change` (a mismatch is a protocol error: log it and re-query with
`list-windows`). We compute it when we send a layout back (`select-layout`, which rejects a bad
checksum, `layout-custom.c:183-191`).

Mapping onto `TerminalPanes::Node` (`terminalpanes.h:192-204`): `{` → split with
`orientation = Qt::Horizontal` (side by side), `[` → `Qt::Vertical`, leaf → pane. tmux nodes can
have 2+ children; ours already can (`children`, `fractions`). In tmux mode the node additionally
stores its **cell rectangle** and the layout walk uses cells, not fractions (2.3).

Reconciliation on `%layout-change` keeps `TerminalView`s by pane id: a pane that moved inside the
tree keeps its view, scrollback and selection; new ids get new views; vanished ids are destroyed
(their pane is gone from this window; if the id shows up in another window's layout it was moved
there, see 2.5, and the view moves with it instead of being destroyed: the reconciler runs over
all windows of the session before deleting anything).

`window_visible_layout` differs from `window_layout` only while zoomed: it is the single zoomed
pane at full size. `window_raw_flags` (`format.c:2982-2985`, `window.c:979-994`) contains `Z` when
zoomed, `*` for the current window.

Floating panes (new in 3.7, CHANGES:29-44) appear in `<…>`. 3.7c itself cannot restore them through
`select-layout` (`layout_parse` stops at `>` and then requires the end of the string,
`layout-custom.c:204-207`). Phase 1 shows them as ordinary leaves overlaid at their rectangle, or
ignores them with a notice (open question Q7).

### 2.3 Geometry: cells, borders, and our dividers

tmux sizes the **window** (from the control client's size, 3.4) and splits it with 1-cell borders.
Our panes are pixel-laid-out with a 6 px divider (or `splitGap`), fractions per child. In tmux mode:

- `TerminalTabs` computes the client size as `floor(width / cellW) × floor(height / cellH)` of the
  panes area and sends it (3.4). Every tab of the session shares that size (`window-size latest`,
  `options-table.c:1537-1541`), which is right: all our tabs share one area.
- `TerminalPanes` lays out leaves at `x = cellX * cellW`, `y = cellY * cellH`, `w = cellsW * cellW`,
  `h = cellsH * cellH`. A vertical border is one column (≈ 9 px at our default font): the divider
  item is drawn in it. A horizontal border is one row (≈ 18 px): **proposal** — the pane header
  (`PaneHeader`, which already sits at the top of every cell) occupies that row for the pane below
  it, and the client height is reduced by one row so the top panes get a header row too. This makes
  our chrome land exactly on tmux's borders instead of leaving gray gaps (open question Q3).
- `TerminalView::applyGrid` (`terminalview.cpp:520-544`) derives rows/cols from its pixel size.
  In tmux mode the grid is **imposed** (`setImposedGrid(rows, cols)`), because the layout is the
  authority; the item's pixel size matches it by construction. If the font changes, we recompute
  the client size, send it, and wait for the new layout.
- If tmux imposes a size smaller than our area (another, smaller client attached, 5.4), the unused
  margin is painted with the window background; if larger, the panes are clipped at the right and
  bottom (iTerm2 shows the same "gray areas").

### 2.4 Active window / pane, titles

- Server → us: `%session-window-changed $S @W` selects tab W; `%window-pane-changed @W %P` sets
  the focused leaf of tab W (without stealing keyboard focus from another OS window).
- Us → server: clicking a tab sends `select-window -t @W`; focusing a pane sends
  `select-pane -t %P`. To avoid echo loops each side records the last value it sent and ignores the
  matching notification.
- Focus-follows-mouse (`TerminalPanes::focusOnHover`) would send `select-pane` on every hover. We
  send it **lazily**: hover changes only our focus; `select-pane` goes out before the first key or
  click sent to a pane that is not tmux's active one, and when the window loses activation. tmux's
  active pane matters only for commands without `-t` and for other clients.
- Tab title: today it is the focused pane's OSC title (`TerminalTabs::Tab::title`). The OSC 0/2
  bytes are in `%output`, so this keeps working unchanged. The tmux window name (`%window-renamed`,
  automatic-rename from the running command) is shown when no OSC title is set; a user-set custom
  title is sent as `rename-window -t @W '<name>'` so it persists in tmux.
- OS window title: `<session name> — <focused title>`.

### 2.5 User actions → tmux commands

The tree is **server-driven**: a UI action sends a command and changes nothing locally; the
resulting notifications (`%window-add`, `%layout-change`, `%window-pane-changed`) update the
model. One code path for actions from SolidTerm, from a key binding inside tmux, and from another
client.

| SolidTerm action (`TerminalTabs`/`TerminalPanes` API) | tmux command |
|---|---|
| `split(Qt::Horizontal)` (split right) | `split-window -h -t %P -c '<cwd>'` |
| `split(Qt::Vertical)` (split down) | `split-window -v -t %P -c '<cwd>'` |
| `closeFocused()` | `kill-pane -t %P` |
| `newTab()` | `new-window -a -t @W -c '<cwd>'` (after the current tab) |
| `closeTab(i)` | `kill-window -t @W` |
| `selectTab(i)` / pane focus | `select-window -t @W` / `select-pane -t %P` (lazily, 2.4) |
| `toggleZoom()` | `resize-pane -Z -t %P` |
| divider drag | `resize-pane -t %P -x <cols>` (left/top neighbour of the divider) or `-y <rows>`; coalesced to one command per 50 ms while dragging, final value on release |
| drop on a pane's centre (swap) | `swap-pane -s %A -t %B` |
| drop on a pane's side (re-split) | `move-pane -s %A -t %B -h\|-v [-b]` (`-b` = before, for left/top) |
| drop on a pane of another tab (same session) | same `move-pane` (works across windows) |
| drop on the tab strip ("new tab") | `break-pane -s %A [-a -t @W]` |
| drop on another SolidTerm window of the **same server** | `move-pane` / `break-pane -s %A -t $S2:` (both sessions get notifications) |
| drop across servers, or tmux ↔ local | refused, with an explanation |
| detach a pane to a new OS window | phase 6 (per-window size); until then: `break-pane` into a new tab |
| per-pane font zoom (`zoomFocused`) | not supported in tmux mode (all panes share the cell grid of the window); disabled |
| `setTabTitle` | `rename-window -t @W '<title>'` |

`<cwd>`: `TerminalView::currentDirectory()` reads `/proc/<pid>/cwd` of the pty child
(`terminalview.cpp:2180`). For a tmux pane it is `#{pane_current_path}`, which we keep from the
pane's last `list-panes` / subscription; a split can also pass `-c '#{pane_current_path}'` and let
tmux expand it.

---

## 3. Pane I/O

### 3.1 A pane without a pty: the feed pipe

Today (`terminalview.cpp:485-502`) `PtySession` forks the shell and `TermWorker` reads the pty
master on its own thread (`termworker.h:83`), feeding libvterm under the `ScreenMutex` in 4 KiB
slices (`docs/solidterm-parser-thread.md`).

In tmux mode, nothing forks. **Each tmux pane gets a pipe** (`pipe2(O_NONBLOCK|O_CLOEXEC)`):

```
tmux server ──pipe──► TmuxClient reader thread ──decode %output──► pane %P's pipe (write end)
                                                                     │
                     TermWorker (pane thread) ◄── read end, as if it were the pty master
```

- `TermWorker::start(fd, …)` receives the pipe's read end instead of the master fd: **TermWorker,
  TermScreen, the slices, the news/eventfd path and the renderer are unchanged**. The only cost is
  one extra `write(2)` / `read(2)` copy per chunk through a kernel pipe buffer, which is small
  next to libvterm's parse cost, and it keeps per-pane parsing parallel (one tmux connection, N
  parser threads). This is the "zero-overhead" route: no virtual call, no new branch in the hot
  path.
- The alternative, the reader thread calling every pane's sink directly, would put all panes'
  libvterm work on one thread and make it contend with N `ScreenMutex`es. Rejected.
- Pipe full (pane worker paused or slow): the reader thread appends to that pane's **overflow
  buffer** in user space and adds the pipe to its `POLLOUT` set; it never blocks and never stops
  reading the control stream. The overflow is bounded (section 6.1).
- End of pane: when the pane leaves the session's layouts (or `%window-close` removes its window)
  the reader closes the write end; the worker reads EOF and reports "exited" as it does for a pty.
- `%output` for a pane id we have no view for yet is held in that pane's overflow buffer until the
  view exists (a pane created by another client, or output racing the layout notification).

`TerminalView` gets one pointer, `TmuxPane *m_tmux` (null for a local pane), and branches on it
only at the cold sites: start (`ensureSession`), resize (`applyGrid`), write (`writeToPty`), pause
(`pauseOutputForSelection`), exit, `currentDirectory`. `PtySession` is untouched. A `PaneSource`
interface with virtual calls is not needed: there are exactly two sources and the hot path (the
worker reading an fd) is already source-agnostic.

### 3.2 Query replies must be dropped

libvterm answers DA, DSR, DECRQM… through the output callback while it parses
(`vterm_output_set_callback`, `terminalview.cpp:447-449` → `writeToPty`, `263-271`). tmux has
already answered the same query from its own parser (1.8). Sending ours too would type garbage
into the program. Rule: in tmux mode, bytes the output callback produces **during a feed** (on
the worker, inside `wkFeed`, `terminalview.cpp:206-219`) are dropped. A flag `m_inFeed` set and
cleared in `wkFeed` (lock held) is enough. Keys, mouse, paste and focus reports are produced on the
GUI thread outside a feed, so they pass.

### 3.3 Keyboard

Keys must be encoded the way tmux would encode them for that pane. tmux tracks the pane's modes
(cursor-key mode, keypad mode, `extended-keys`/CSI u, `pane_key_mode`) and answered the program's
mode queries, so tmux is the authority. Also tmux's TERM differs from ours: inside tmux,
`TERM=tmux-256color` (or `screen*`; `options-table.c:351-356`, `configure.ac:106-112, 918-919`),
whose terminfo says `khome=\E[1~`, `kend=\E[4~`, and tmux sends exactly those
(`input-keys.c:109-114`). libvterm, an xterm encoder, sends `\E[H`/`\EOH` for Home. A program that
trusts terminfo would not recognise our Home.

So in tmux mode:

| input | sent as | why |
|---|---|---|
| text (printable, no modifiers, IME commits) | `send-keys -t %P -l '<utf-8 run>'` (`cmd-send-keys.c:135-148`) | literal; consecutive keystrokes coalesce into one command |
| special keys and anything with Ctrl/Alt (arrows, Home/End, PgUp/PgDn, Ins/Del, F1-F24, Tab/BTab, Enter, Escape, BSpace, `C-a`, `M-x`, `C-S-Left` …) | `send-keys -t %P <tmux key name>` | tmux encodes per the pane's modes (`window_pane_key` → `input_key_pane`) |
| mouse reports (wheel/press/drag in mouse mode) | `send-keys -t %P -H <hex bytes>` produced by libvterm | `-H` bytes are written raw (`cmd-send-keys.c:116-122` → `input-keys.c:584-589`); tmux has no way to inject a mouse event for an unbound key. libvterm's mouse mode comes from the same raw output tmux saw, so it agrees |
| paste | `send-keys -t %P -H …` of the payload, wrapped in `\e[200~`/`\e[201~` when the pane has bracketed paste on (libvterm's state, from the raw stream) | raw; chunked to ≤ 1 KiB of payload per command (3 bytes per payload byte in hex) |
| focus in/out (DECSET 1004) | `send-keys -t %P -H 1b 5b 49` / `… 4f` from `updateFocusReport()` | tmux does not send focus events for control clients (it never receives them from us), so there is no duplicate |

Implementation: `keyToVTerm` (`terminalview.cpp:943`) gets a tmux branch that maps the Qt key +
modifiers to a tmux key name (a static table, the same set libvterm handles). All of these are
produced on the GUI thread and appended to the client's command queue in order, so a fast
`ab<Home>` keeps its order. `readOnly` blocks them locally as today; `attach -r` makes the whole
client read-only (`cmd-send-keys.c:171-174`).

If the pane is in a tmux mode (copy mode, from a key binding inside tmux or another client), keys
sent with `send-keys` go to the mode's key table (`cmd-send-keys.c:87-101`). See 6.6.

### 3.4 Resize

- Client size: `refresh-client -C <cols>x<rows>` (`cmd-refresh-client.c:117-130`). A control client
  is ignored for sizing until it has set a size (`resize.c:91-94`), and control clients have no
  status line (`status.c:259`, `resize.c:451`), so the window gets exactly our cells.
- Per-window size (phase 6, a tmux window shown in a second OS window):
  `refresh-client -C @W:<cols>x<rows>`, cleared with `-C @W:` (`cmd-refresh-client.c:90-115`);
  documented at `tmux.1:1476-1487`. With the default `window-size latest` the client's per-window
  size is used instead of its global one (`resize.c:174-187`).
- We debounce window resizes (one `refresh-client -C` per 30 ms while dragging the window edge),
  then tmux recomputes every window's layout and sends `%layout-change` for each.
- **Resize ordering.** A `%layout-change` that changes pane P's size is followed by output the
  program produced *after* it got SIGWINCH, i.e. for the new size. If P's worker parses that output
  before the GUI has applied the new grid, lines wrap at the old width. Since the reader thread sees
  the notification in stream order, it **fences** the panes whose size changed: their `%output`
  goes to the overflow buffer until the GUI has called `vterm_set_size` for them (the same
  synchronous hand-off `applyGrid` already does, `terminalview.cpp:534-541`), then the fence is
  lifted and the buffer flushed. The fence is per pane, so other panes keep streaming. (A local pty
  has the same race and simply lives with it; here it is fixable for free.)

---

## 4. Initial state and scrollback on attach

### 4.1 What we have to reconstruct

On attach tmux sends a pane's output only from "now": a control client's per-pane offset starts at
the pane's current offset (`control_add_pane`, `control.c:256-262`). The screen, the scrollback and
the terminal modes before that live only in tmux's grid. We rebuild them by **synthesising a byte
stream** for our own parser: reset, history lines, screen lines, cursor position, modes. Feeding
that stream through libvterm at the pane's width means `TermScreen` builds rows, wrap flags and the
scrollback ring the normal way (no new TermScreen API).

### 4.2 Commands (one line, one group, executed back to back)

For pane P, with N = min(`#{history_size}`, our scrollback limit):

```
display-message -p -t %P '<fmt>' ; capture-pane -p -t %P -e -C -J -S -N -E - ; capture-pane -p -t %P -P -C
```

- `<fmt>` gathers the pane state (`format.c:3185-3764`): `#{pane_width} #{pane_height}
  #{history_size} #{cursor_x} #{cursor_y} #{cursor_flag} #{alternate_on} #{alternate_saved_x}
  #{alternate_saved_y} #{scroll_region_upper} #{scroll_region_lower} #{keypad_cursor_flag}
  #{keypad_flag} #{insert_flag} #{wrap_flag} #{origin_flag} #{mouse_standard_flag}
  #{mouse_button_flag} #{mouse_any_flag} #{mouse_sgr_flag} #{mouse_utf8_flag} #{bracket_paste_flag}
  #{cursor_shape} #{cursor_blinking} #{pane_in_mode} #{pane_current_path} #{pane_pid}`, then
  `#{pane_title}` last (it may contain spaces). N needs `history_size`, so the very first attach
  sends `display-message` alone for all panes (one group), then the capture groups.
- `-e` emits SGR (and OSC 8 hyperlink) sequences for cell attributes; `-C` escapes non-printables as
  `\ooo` and the backslash as `\\` (`cmd-capture-pane.c:233-236`, `grid.c:1135-1141`), so the
  reply survives the line protocol and decodes with the same decoder as `%output` (plus the `\\`
  form). `-J` joins wrapped lines and keeps trailing spaces (`tmux.1:2697-2707`): fed back at the
  same width, they re-wrap into the same rows with the right wrap flags. `-S -N` / `-E -` select the
  last N history lines through the bottom of the screen (`cmd-capture-pane.c:191-230`; `-S -`
  would mean the whole history).
- `-P` returns any **incomplete escape sequence** tmux's parser is in the middle of
  (`cmd-capture-pane.c:79-100, 325-326`). Our parser must be in the same state, or the next
  `%output` byte would be misread. It is fed last.
- When `alternate_on=1`, tmux's grid holds the history plus the *alternate* screen, and the normal
  screen is a copy in `saved_grid` without history (`screen.c:676-706`). The group is then:
  history (`-S -N -E -1`, only if `history_size > 0`), the saved normal screen
  (`capture-pane -a -q -e -C -N`, `cmd-capture-pane.c:168-176`), the alternate screen
  (`capture-pane -e -C -N -S 0 -E -`), and `-P`. We feed history + normal screen, then
  `\e[?1049h` (switch, save cursor at `alternate_saved_x/y`), then the alternate screen row by row
  with absolute positioning (`CSI r;1H` + row, no `-J`: the alternate screen needs exact rows, not
  wrap flags).
- Every capture uses `-q` where applicable and the group contains nothing that can fail on a live
  pane, because a failure would silently drop the rest of the group (1.2). On `%error` we discard
  the group's remaining entries, mark the pane "unseeded" and retry once.

Synthesised stream, in order: `RIS`; history and screen lines separated by `\r\n` with no newline
after the last screen row (so the last captured row is the bottom row); `CSI <cy+1>;<cx+1> H`;
`DECSTBM` from `scroll_region_*`; `DECSET/DECRST` for 1 (cursor keys), 4 (insert, as `SM/RM 4`),
6 (origin), 7 (autowrap), 25 (cursor), 1000/1002/1003/1005/1006 (mouse), 2004 (bracketed paste);
`DECKPAM/DECKPNM`; `DECSCUSR` from `cursor_shape`/`cursor_blinking`; `OSC 2;<title>`; then the `-P`
bytes. The reader thread writes this into P's feed pipe **in stream position** (4.3).

Not recoverable from formats (they stay at defaults until the program sets them again): focus
reporting (1004), character sets (G0/G1), custom tab stops, the OSC 4 palette, kitty/CSI-u keyboard
flags (tmux encodes keys anyway, 3.3), the saved cursor (DECSC) on the normal screen, and the SGR
pen in effect at the cursor (`-e` restores cell attributes, not the pen a program will write with
next; programs that rely on a pen set before the attach are rare and repaint).

### 4.3 Consistency between the capture and later `%output`

The rule, from the facts in 1.2 and 1.4:

1. `%output` for P that arrives **before** the group's first `%begin` is already contained in the
   capture: tmux parsed it into the grid in the same read callback that queued it
   (`window.c:1158-1162`), and the reply lines sit behind those output blocks in the client queue
   (`control.c:29-43, 411-435`).
2. No `%output` can appear between the blocks of one group: all commands of the line run in one
   `cmdq_next` pass (`cmd-queue.c:730-800`) with no pane read in between, and blocks are never
   interleaved with notifications (`tmux.1:8154`).
3. `%output` after the group's last `%end` is new.

So the reader thread marks P "seeding" when P becomes known, **drops** P's `%output` while seeding,
writes the synthesised stream when the group's last block completes, and from then on forwards P's
output normally. All of this happens on the reader thread, in stream order, so there is no race to
reason about. Panes created while we are attached need no seeding: their first output is sent from
the pane's start, so we only hold it until the view exists.

### 4.4 Reseed after `%pause` (or after a hold overflow)

When a pane was paused (1.6, 6.1), its output in between is lost to us. Resume and snapshot **in
one group**: `refresh-client -A '%P:continue' ; display-message … ; capture-pane …`. `continue`
moves the offset to the pane's current position (`control.c:372-374`) at the same instant the
capture reads the grid, so the new stream starts exactly where the snapshot ends. Our scrollback
keeps what it had, then a marker row ("output skipped while paused — it is in tmux's history"),
then the fresh screen. Re-fetching the skipped lines into our scrollback is possible (they are in
tmux's history) but needs an "insert before" path in TermScreen; not in the first version (Q5).

### 4.5 Cost

A 50 000-line history captured with `-e -C` is a few MB per pane on the control pipe and about
70 ms of libvterm parse at the measured rates (docs/evidence/solidterm). Attach shows each pane as
soon as its own seed is parsed. tmux's `history-limit` defaults to 2000 lines
(`options-table.c:677-684`); since 3.7 changing it applies to existing panes too
(`options.c:1260-1263` → `session_update_history`, `session.c:765-790`). The SolidTerm-managed setup
(section 10) raises it to match our scrollback limit, otherwise a reattach can only restore 2000
lines.

---

## 5. Detach and reattach

### 5.1 Closing and reopening

| user action | tmux mode effect |
|---|---|
| close the OS window | `detach-client` (or the empty line, `control.c:567-571`); tmux replies `%exit` and the client process exits. Sessions, programs and history stay in tmux. Views are destroyed after `%exit` (or a 2 s timeout, then SIGTERM to the client) |
| quit SolidTerm | every `TmuxClient` detaches the same way |
| SolidTerm crashes or is killed | the client process gets EOF/EPIPE on its fds; the server drops the client (`control_error_callback`, `control.c:542-549`). Nothing in tmux is lost |
| `solidterm --tmux S` again | attach, rebuild tabs/splits from `list-windows` / `%layout-change`, seed every pane (4) |
| close a tab / a pane | `kill-window` / `kill-pane` (programs end, as with local tabs today). A confirmation is shown when the pane runs something other than a shell (Q4) |
| "Kill session" (menu) | `kill-session -t $S`, then `%exit` |
| last window of the session exits | `detach-on-destroy` (default on) detaches us (`%exit`); the OS window closes, as a local window closes when its last shell exits |

Initial attach sequence:

```
spawn  tmux -C new-session -A -s S -x C -y R          (flags-0 block for the argv command, ignored)
recv   %session-changed $S S
send   refresh-client -C CxR
send   refresh-client -f pause-after=5
send   refresh-client -r '%P:…'  (per pane, after list-panes)      (1.8)
send   list-windows -F '#{window_id} #{window_index} #{window_active} #{window_raw_flags} #{window_layout} #{window_visible_layout} #{window_name}'
send   list-panes -s -F '#{pane_id} #{window_id} #{pane_active} #{pane_width} #{pane_height} #{pane_current_path}'
       → build tabs + split trees + views (all panes "seeding")
send   per pane: the seed group (4.2)
```

`%layout-change` and `%window-add` that arrive while the initial lists are in flight are applied on
top in stream order; the reconciler is idempotent.

### 5.2 Several sessions, several windows

One OS window per session, one `TmuxClient` per OS window. A second `solidterm --tmux S` for a
session that already has a SolidTerm window **raises that window** instead of attaching a second
control client (single instance knows every live `TmuxClient`).

### 5.3 The user also attaches a normal tmux client

Allowed, with tmux's semantics, which the UI must not hide:

- Clients attached to the same session share its current window. If the other client switches
  window, `%session-window-changed` switches our tab too. Users who want independent views attach
  the other client through a **session group** (`tmux new-session -t S`); its window changes then
  arrive to us as `%unlinked-window-*` / nothing.
- `window-size latest` (`options-table.c:1537-1541`): the window takes the size of the most recently
  active client, so typing in the other client can shrink or grow our panes; we render at tmux's
  size with margins or clipping (2.3). A preference may set `window-size largest` or
  `ignore-size` on the other side; we do not change server options behind the user's back.
- Flow control: a normal client keeps the pane's pty read even when we are not consuming
  (`server-client.c:1684-1687`), so holds on our side (6.1) never block programs while a normal
  client is attached.
- Commands typed in the other client (`split-window`, `kill-pane`…) arrive as notifications and
  just work, because our tree is server-driven (2.5).

---

## 6. Interaction with existing SolidTerm features

### 6.1 Pause on selection

Today a selection drag stops reading the pty, so the program blocks on a full kernel buffer
(`ptysession.h:38-48`; threaded: `TermWorker::setPaused`, `terminalview.cpp:1812-1840`). Over tmux
there is no lossless per-pane block from one control client: stopping the control pipe stalls
**every** pane of the session (and only blocks programs if no other client is attached), and
`%P:pause` / `pause-after` **discard** data (`control.c:385-390`).

Design: the pane's `TermWorker` pauses exactly as today, its feed pipe fills, the reader thread
keeps the rest in the pane's overflow buffer (3.1). The text under the pointer does not move, which
is what the owner asked for; the difference is that the **program is not blocked**, it keeps
running and its output waits in our memory. Cap: 8 MiB per pane (configurable). Past the cap, send
`refresh-client -A '%P:pause'` (tmux discards from then on) and on release reseed (4.4). A drag
rarely lasts long enough to matter; `seq 1 1e9` during a long drag would hit the cap.

Alternative, closer to "stdout must block": while any pane of the session holds a selection, stop
reading the control pipe entirely (all panes freeze, programs block once tmux's own buffer fills,
`server-client.c:1739-1745`), with `pause-after` as the safety valve. It freezes unrelated panes,
so it is not recommended. Owner decision (Q1).

### 6.2 Scrollback, wheel, search, links, selection

All native, unchanged: they read `TermScreen` (`docs/solidterm-screen-buffer.md`, "Readers"), which
holds the program's output exactly as with a pty plus the seeded history. The wheel scrolls our
scrollback unless the program asked for mouse reporting, in which case the report goes out with
`send-keys -H` (3.3). On the alternate screen without mouse mode, `sendAltScrollKeys` sends arrows
(as key names, 3.3). Clear scrollback clears **ours**; the menu offers "also clear tmux history"
(`clear-history -t %P`).

### 6.3 Pane drag across windows

Covered in 2.5. The drop path records the target as today (`TerminalPanes::s_pending`,
`terminalpanes.h:255-263`); for tmux panes `finishDrag` issues the command instead of moving the
`TerminalView`. When the notifications arrive, the reconciler moves the existing view into the
target tree (by pane id), so scrollback, selection and the parser state ride along: nothing is
reseeded.

### 6.4 Focus follows mouse

Local only, `select-pane` sent lazily (2.4).

### 6.5 Preferences

Style properties (font, colours, palette, opacity, blur, scrollbar mode) apply to tmux panes
exactly as to local ones (`TerminalPanes::applyStyle`). Exceptions: per-pane font zoom is disabled
in tmux mode (2.5); a font size change resizes the client (2.3); a theme change re-sends the OSC
10/11 colours (1.8). New preferences: tmux binary path, `pause-after` seconds, overflow cap, seeded
history lines, "close window = detach | ask | kill session".

### 6.6 tmux modes (copy mode, choose-tree…)

If a pane enters a tmux mode (from a binding typed through `send-keys`, or from another client),
tmux draws the mode on its own grid, **not** in the pane's output: we never see it in `%output`.
`%pane-mode-changed %P` (`control-notify.c:39`) tells us; `#{pane_in_mode}`/`#{pane_mode}` say
which. We show a badge on the pane ("tmux: copy-mode — Esc to leave") and route keys to it as
usual; a "Leave tmux mode" action sends `send-keys -X -t %P cancel`. We never enter tmux modes
ourselves: our own selection and search replace them. The prefix key still reaches tmux's key
table only if we send it as a key name; we do not interpret tmux key bindings locally.

### 6.7 Single instance

`SingleInstance::Request` carries only `cwd` and `env` (`singleinstance.h:25-28`), and any
`SOLIDTERM_*` variable other than `SOLIDTERM_DIR` forces standalone (`singleinstance.cpp:53-63`).
So `--tmux` must travel as an **argument**: extend `Request` with `args` (a QStringList) and
`openForwardedWindow` (`main.cpp:119-143`) opens a tmux-mode window when `--tmux` is present. The
server side keeps the registry of live `TmuxClient`s (5.2).

---

## 7. Failure modes

| case | detection | behaviour |
|---|---|---|
| tmux not installed | `posix_spawn` fails / `tmux -V` fails | the menu items are disabled with a tooltip; `--tmux` opens an error window with the reason |
| tmux too old | `tmux -V` → `tmux 3.7c`; parse major.minor | **minimum 3.5** (needs `pause-after`/`-A`/`%extended-output` from 3.2, `%config-error` from 3.4, `refresh-client -r` from 3.5). Older: refuse with the version found. Feature-gate anything newer (floating panes, 3.7) on the version |
| `$TMUX` set (SolidTerm itself runs inside tmux) | — | unset it for the child (1.1) |
| no such session (`attach -t S`) | `%exit` right after spawn, stderr text | error notice with "Create it?" |
| protocol error (unknown notification, bad checksum, a `%end` that matches nothing, a reply for an empty queue) | parser | unknown notifications are logged and ignored (future-proof, the man page reserves extra args); a desynchronised command queue resets: drop pending callbacks, re-run `list-windows`/`list-panes`, reseed panes whose state is uncertain |
| `%error` reply | correlator | the callback gets the error text; user-initiated commands show it as a transient notice (e.g. "pane too small" from `split-window`) |
| tmux server killed | `%exit server exited unexpectedly` / EOF | the window shows a banner "tmux server ended"; panes go read-only with their content kept (like a dead local shell), "Close" and "Reattach" buttons |
| our client killed by someone (`tmux detach-client`, `kill -9`) | `%exit detached` / EOF | same banner, "Reattach" |
| output burst | 1.6 | fair per-pane scheduling on tmux's side (`control.c:744-764`); `pause-after` prevents the 300 s disconnect; per-pane overflow cap on ours |
| huge single line / long paste | — | commands chunked (3.3); the pipe writer is non-blocking with a queue |
| Unicode width disagreement | — | see below |
| `TERM` inside tmux | — | see below |
| programs wrapping sequences for tmux (`\ePtmux;…\e\\` passthrough) | they see `$TMUX` | tmux would unwrap them (if `allow-passthrough`); we get them **raw** in `%output` and libvterm ignores the DCS. Phase 2: unwrap `DCS tmux;` in tmux mode and feed the inner sequence (OSC 52, hyperlinks, images) |

**Unicode widths.** `%output` is the raw byte stream, so *our* libvterm decides widths for what we
draw (confirmed in 1.4: tmux queues the bytes before its own parser runs). tmux keeps its own grid
with its own widths: `utf8proc_wcwidth` or libc `wcwidth`, overridden by a built-in table and the
`codepoint-widths` option (`utf8.c:53-60, 553-580`). Where the two disagree, two things diverge:
(1) the seed (4.2) is tmux's rendering while later output is ours (one-time difference, fixed by the
next repaint); (2) programs that query the cursor position (DSR 6n) get **tmux's** answer
(`input.c:1703-1707`) and may place text by it. This is the same class of mismatch as an app whose
`wcwidth` differs from the terminal's; it does not accumulate (each redraw re-anchors with absolute
moves). Mitigation: align `codepoint-widths` with libvterm's table for the known disagreements
(emoji presentation sequences), measured with a width corpus in phase 3.

**TERM inside tmux.** Panes get `default-terminal` (`tmux-256color` when the build host had it,
else `screen`, `configure.ac:918-919`). Programs emit sequences per that terminfo, which libvterm
(xterm-like) parses: `tmux-256color` is a subset of xterm's repertoire (italics `sitm`, `Ss/Se`
cursor style, `smxx` strike, …). True colour depends on the program's own detection (`COLORTERM`);
tmux passes 24-bit SGR through to us raw anyway. To verify in the phase-3 integration run: vim,
htop, less, lazygit, claude (its TUI) under `tmux-256color`.

---

## 8. UX

- **Start:** `solidterm --tmux [session]` (default name: `main`) attaches or creates
  (`new-session -A`); `solidterm --tmux-attach <session>` attaches only. Menu (window context menu
  and pane menu): **tmux ▸ New session…** (name, start directory), **Attach session ▸** (live list
  from `tmux list-sessions -F '#{session_id} #{session_name} #{session_windows} #{session_attached}'`
  run as a one-shot process), **Detach**, **Kill session…**, **Rename session…**.
- **Indicator:** the tab bar shows a small "tmux" badge with the session name at its left edge; a
  pane in a tmux mode shows the mode badge (6.6); a paused or held pane shows the existing
  `outputPaused` tint (`terminalview.h:94`).
- **Window title:** `<session> — <focused title>`, so sway's window list shows which session a
  window holds.
- **Close:** closing a tmux window detaches by default; the preference can ask or kill.
- **Agents:** `solidterm --tmux <project>` is what the agent scripts call (section 10). Optional
  preference "new windows open in tmux session X" for a tmux-by-default workflow (Q6).
- **tmux-specific actions** in the pane menu: "Clear tmux history", "Leave tmux mode", "Break pane
  to new tab", "Rename window".

---

## 9. Phased plan

Effort in focused agent-days, including tests. Every phase lands green on its own; phases 1-4 are
needed before the owner uses it daily.

### Phase 0: protocol library (Qt-free) — 2 days

`apps/solidterm/native/tmux/`: line splitter; `%output` decoder (octal + `\\`); notification parser
(the table in 1.3); block correlator (FIFO, flags-1 only, groups, `%error` drops the rest of the
group); layout parser/printer with checksum; key-name table (Qt key + modifiers → tmux name);
command builder with quoting.

Tests (unit, no tmux): golden transcripts written from the man page and the source (every
notification, split `%output` lines ending mid-UTF-8 and mid-escape, `%begin`/`%error` with a parse
error, a two-command group whose first command fails, flags-0 blocks interleaved), layout strings
with nested `{}`/`[]`, three-way splits, floating `<…>`, bad checksums; checksum vectors computed
by real tmux (recorded in phase 3, checked in as fixtures).

### Phase 1: read-only viewer — 4 days

`TmuxClient` (spawn, reader thread, command queue, eventfd to the GUI like `TermWorker`'s news),
session model, tabs and split trees from layouts, cell-exact layout in `TerminalPanes`, imposed grid
in `TerminalView`, feed pipes into `TermWorker`, seeding (4.2-4.3), resize (`refresh-client -C`) with
the resize fence, `pause-after` and reseed.

Tests: a **fake tmux**: a small scripted control-mode peer (C++ test binary or Python script) that
speaks the protocol over stdin/stdout and plays a scenario (attach with 2 windows × 3 panes, seed
replies, a `%layout-change`, an `%output` burst, `%pause`, `%exit`). SolidTerm runs it in place of
tmux via `SOLIDTERM_TMUX_BIN` (a debug variable, so standalone, `singleinstance.cpp:53-63`). Checks:
TermScreen contents of each pane equal an oracle (the same bytes fed to a bare TermScreen), seeding
rules (output before `%begin` dropped, after `%end` kept), fence ordering. Real window: a
self-quitting hook like `SOLIDTERM_SHOT` grabs the window after the scenario; compared with a
reference grab.

### Phase 2: input — 2 days

Keys (names / `-l` / chords), mouse (`-H`), paste (chunked, bracketed), focus reports, OSC 10/11
report, dropping in-feed query replies, `DCS tmux;` unwrap.

Tests: the fake tmux records the commands it receives; assert the exact command lines for a key
corpus (every special key × modifiers, IME commit, a 100 KB paste, wheel in mouse mode). No reply
bytes leak during a feed of DA/DSR queries.

### Phase 3: structural sync and real tmux — 4 days

Every row of the 2.5 table, active window/pane sync without echo loops, zoom, divider drag,
drag-and-drop to tmux commands with view reuse, window/session rename, `%pane-mode-changed` badge.
**Install tmux 3.7c** and run the integration suite against a real server on a private socket
(`tmux -L solidterm-test`, `-f /dev/null`): split/kill/move/swap/break/zoom/resize round trips,
attach-seed-detach-reattach equality (our TermScreen before detach vs after reattach, history
included), a second normal client switching windows and resizing, `kill-server` mid-session, a
`seq 1 5000000` flood with `pause-after=1` and a stalled GUI (SIGSTOP the GUI thread's reader for
2 s), real programs (vim, htop, less, lazygit, claude) grabbed in the real window.

### Phase 4: lifecycle and UX — 3 days

`--tmux` / `--tmux-attach` through single instance (`Request.args`), menus, session picker,
detach-on-close, banners for server exit, version gate and error windows, preferences,
pause-on-selection hold with cap and reseed. Tests: forwarded `--tmux` opens/raises the right
window (two processes), closing the window leaves the session alive (`tmux has-session`), the
version gate with a fake `tmux -V`.

### Phase 5: agents migration — 1 day

Section 10, with a dry run on one project first.

### Phase 6 (later): remote and gateway — 4 to 6 days

`ssh -T host tmux -C …` as a `TmuxServer` transport (no pty needed); the in-pane `\eP1000p` gateway
for a hand-typed `tmux -CC`; per-window sizes for showing one session's windows in several OS
windows (detach a tmux tab to its own OS window).

**Total: about 16 days for phases 0-5, plus 4-6 for phase 6.**

### Risks

| risk | mitigation |
|---|---|
| seeding is not byte-exact (modes not exposed as formats, tmux-vs-libvterm rendering of the capture) | accept a one-time cosmetic difference; full-screen programs repaint; phase-3 attach/reattach equality test measures it |
| command/notification races in the reconciler (another client acting while we act) | server-driven model only: we never mutate the tree optimistically |
| key encoding differences vs. local panes | key names make tmux the encoder; phase-2 corpus test |
| pause-on-select semantics differ (program not blocked) | owner decision Q1; cap + reseed |
| geometry: one-cell borders vs our dividers/headers | header-in-border proposal (Q3); worst case iTerm2-like gaps |
| tmux evolves the protocol | unknown notifications ignored, version gate, extra args before ` : ` ignored as specified |
| memory: per-pane overflow buffers | bounded, then `%P:pause` + reseed |

---

## 10. Migrating the agents from GNU screen to tmux

Today (`~/.local/bin`, read for this design, not changed):

- `agent-windows` opens one SolidTerm window per detached **screen** session named after the
  project, with `SHELL=agent-attach` and `AGENT_SCREEN=<name>` in the environment (forwarded by
  single instance), and moves the new sway container to the project's workspace (`MAP`: name →
  workspace, e.g. `solid-qml-native → 1`, `photo-wagon → 6`).
- `agent-attach` runs `screen -x $AGENT_SCREEN` once, then a login fish.
- `agent-rescreen` moves an idle agent's Claude session into a fresh screen of the same name and
  resumes it with `claude --resume <sid> --remote-control <name>`.

With tmux control mode:

| today | tmux |
|---|---|
| `screen -dmS <name> zsh -c '… claude --resume <sid> --remote-control <name>'` | `tmux new-session -d -s <name> -c <cwd> '/opt/claude-code/bin/claude --resume <sid> --remote-control <name>; exec zsh'` |
| detection: `screen -ls` "(Detached)" | `tmux list-sessions -F '#{session_name} #{session_attached}'`; "already has a SolidTerm window": `tmux list-clients -t <name> -F '#{client_control_mode}'` (`format.c:3224`) |
| `agent-windows` spawns `solidterm` with `SHELL=agent-attach` | spawns `solidterm --tmux-attach <name>` (no shell indirection; `agent-attach` is retired). Tabs/splits added later in that window are tmux windows/panes of the agent's session, so they persist too |
| sway placement by new `con_id` | unchanged: same `swaymsg … move container to workspace` after the window appears; the window title (`<name> — …`) also allows `[title="^<name> "]` criteria instead of the before/after diff |
| `agent-rescreen` (fresh screen to pick up `~/.screenrc`) | `agent-retmux`: when idle, `tmux respawn-pane -k -t <name>:0.0 '<claude --resume … --remote-control …>'` (same pane, same window, no new viewer needed). The `termcapinfo` problem that motivated rescreen does not exist: tmux reads `~/.tmux.conf` on server start, and SolidTerm does not care about the outer terminal capabilities tmux would use for a tty client |
| `SolidTerm` restart: rerun `agent-windows` | same, or SolidTerm restores its tmux windows itself on start (optional: remember `{session → workspace}` and reattach; Q6) |

Server configuration the scripts create once (`~/.config/solidterm/tmux.conf`, loaded with
`tmux -f` only when SolidTerm starts the server; the user's own `~/.tmux.conf` stays in charge
otherwise):

```
set -g history-limit 50000       # match the SolidTerm scrollback we want back after a reattach
set -g default-terminal tmux-256color
set -g allow-passthrough on      # programs' DCS passthrough reaches the raw stream unchanged anyway
set -g status off                # only matters for normal clients; control clients have none
```

The Remote-Control Claude sessions are unaffected: they only need a pty, which tmux provides. One
behaviour changes: a Claude TUI now sees `TERM=tmux-256color` and `$TMUX`, as it would in any tmux.

Order: migrate one project (`office`, workspace 2) end to end, run it for a day, then the rest. The
screen sessions are left alone until each agent is idle (the same idle check `agent-rescreen`
does).

---

## 11. Open questions for the owner

- **Q1 Pause on selection.** Over tmux a lossless block of one pane is not available. Proposed:
  the pane freezes visually and its output waits in our memory (program keeps running), capped,
  then tmux pause + reseed. Alternative: freeze the whole session (all panes) while selecting.
  Which?
- **Q2 Close semantics.** Closing a tmux window detaches (sessions live on). Closing a tab or pane
  kills it (programs end), like local tabs. Confirm, or should closing a tab only detach it
  (hide) as iTerm2's "buried" sessions do?
- **Q3 Borders.** Use the one-row tmux border as the pane header of the pane below it (exact fit,
  needs one row less client height), or keep our pixel dividers and accept gray gaps?
- **Q4 Kill confirmation.** Ask before `kill-pane`/`kill-window` when the pane runs a non-shell
  program (an agent)?
- **Q5 Lost output after a pause.** First version shows a marker row; re-fetching the skipped lines
  from tmux's history into our scrollback is extra TermScreen work. Needed?
- **Q6 Default mode.** Should every new SolidTerm window be a tmux session (auto-named), or only
  windows opened with `--tmux`/the menu? Should SolidTerm reattach its tmux windows on start?
- **Q7 Floating panes (tmux 3.7).** Show them as overlays, or ignore with a notice?
- **Q8 Minimum version.** 3.5 (as proposed) or require 3.7c exactly, which the owner is installing?

## Owner decisions (2026-10-13)
1. Pause-on-select: freeze only the VIEW — the program keeps running, output is buffered per pane (capped) and
   shown on release; on cap overflow fall back to tmux pause + recapture.
2. Closing a tab/pane of a tmux session only HIDES it (iTerm2 "buried"): the pane stays alive in tmux and
   reappears on reattach; a "Hidden panes" menu brings it back. (Kill stays available as an explicit action.)
3. Default mode: opt-in (`--tmux`, "New tmux session…") + on start SolidTerm reattaches the tmux sessions IT
   created.
4. Pane borders: tmux's one-row border becomes the lower pane's native header (exact fit).
Defaults taken for the minor questions (owner may revisit): output skipped while paused → marker row first,
history re-fetch later; floating panes (3.7) ignored with a notice; minimum tmux 3.5.

## Implementation status: phases 0 and 1 (2026-10-13)

Code: `apps/solidterm/native/tmux/` (Qt-free: `protocol` parser, correlator and quoting, `layout`,
`seed`, `client`), `tmuxsession.*` (GUI side), tmux branches in `TerminalTabs`/`TerminalPanes`/
`TerminalView`. Run: `solidterm --tmux <session>` (read-only; the window title says so).

Tests (`meson test`): `solidterm-tmux-protocol` (golden transcripts, one recorded from a real 3.7c
by `tests/tmux/record-transcript.sh`), `solidterm-tmux-client` (the client against the scripted fake
`tests/tmux/fake-tmux.py` + `scenario-basic.py`), `solidterm-tmux-ui` (the real UI offscreen against
the fake, `SOLIDTERM_TMUXTEST=fake`), and `solidterm-tmux-client-real` / `solidterm-tmux-ui-real` (a
real tmux on a private `-S` socket under the build dir, skipped without tmux).

Checked against real tmux 3.7c. These corrections to the sections above are now handled:

- **A killed window of OUR session arrives as `%unlinked-window-close`**, not `%window-close`
  (`kill-window` and a window whose last pane exits both do this). Both remove the tab.
- **`%window-add` carries no layout** and no `%layout-change` follows, so the window is re-listed.
- **Floating panes (3.7) are listed twice**: in `<…>` and as an extra child of the tiled root
  (`{40x24,0,0,0,39x24,41,0[…],40x6,4,2,7}<40x6,4,2,7>`), which breaks `layout_check`. The parser
  drops floating ids from the tiled tree. As decided, they are ignored with a notice.
- `#{alternate_saved_x/y}` read `4294967295` when there is no saved screen. `capture-pane -P` with
  nothing pending answers one empty line. `#{window_raw_flags}` can be empty, so fields are split
  without collapsing spaces.
- **1.4's "a stream parser, so splits are harmless" is wrong for UTF-8.** libvterm 0.3.3 does not
  carry an incomplete UTF-8 sequence across `vterm_input_write` calls; it emits U+FFFD. TermWorker
  now ends every slice on a UTF-8 boundary and holds an incomplete read tail for the next read. This
  fixes the local pty path as well.

Deviations from the plan above:

- Seeding uses the probe, then ONE capture group per pane (4.2). The probe is per pane, not one
  group for all panes, so a pane that dies cannot drop the others' probes.
- The fence (3.4) holds only output that arrives after the size change. Bytes already in the feed
  pipe may still be parsed at the new size (the same race as a local pty).
- `--tmux` makes the process standalone. The single-instance `Request.args` is phase 4.
- `TerminalView` holds one `TmuxLink` pointer plus the imposed grid. It also counts the bytes it has
  parsed (one add per slice) so the tests' oracle can replay resizes at the same stream points.
- Not done in phase 1 (phase 2/3): the key-name table (1.5), OSC 10/11 colours (1.8), select-pane
  echo suppression, and the hidden/buried panes of owner decision 2.

Phase 2 (input) needs: the key-name table (`keyToVTerm` tmux branch); `send-keys -l` coalescing;
`-H` for mouse reports, paste (≤ 1 KiB per command, bracketed per libvterm's mode) and focus
reports; `writeToPty` in tmux mode routes only those. Query replies produced inside `wkFeed` stay
dropped (`m_inFeed`, 3.2). `refresh-client -r` for OSC 10/11. `readOnly` false by default. A
fake-server corpus test of the exact command lines (the fake already logs every line it receives).

## Implementation status: phase 2, input (2026-10-13)

- Keys (`native/tmux/keys.h`): special keys and chords go as tmux key names with `C-`/`M-`/`S-`
  prefixes; plain text and IME commits go as `send-keys -l --`. Consecutive text is coalesced
  until the next event-loop turn, and a key name flushes pending text first, so the order is kept.
  Every name was checked against a real 3.7c with `cat -v`. **3.7c has no `C-BSpace` or
  `C-Escape`: an unknown name is typed as text.** So Ctrl+Backspace is sent as `C-h` (xterm's ^H)
  and Ctrl+Escape drops the Ctrl.
- Mouse reports, focus reports (1004), alternate-screen wheel arrows and paste: libvterm encodes
  them from its own modes (the raw stream gives it the same modes tmux has), and `writeToPty`
  sends the bytes as `send-keys -H`, at most 1 KiB of payload per command. In tmux mode a paste
  goes through `vterm_keyboard_start/end_paste`, so it is bracketed when the program enabled 2004.
  (A local pane's paste is unchanged: still not bracketed.)
- Query replies: `m_inFeed` is set around `vterm_input_write` in `wkFeed`. Output libvterm produces
  while it is set is dropped.
- OSC 10/11: every pane sends `refresh-client -r '%P:ESC]10;rgb:…ESC\'` (and 11) on start and on
  a colour change, without repeats. Checked: a program's OSC 10/11 query then gets SolidTerm's
  colours from tmux.
- `select-pane` goes out lazily, once, before the first input to a pane tmux does not have active.
  The `%window-pane-changed` echo is ignored. A later `%layout-change` no longer moves the local
  focus back to tmux's active pane (only a new tab follows it).
- Panes are writable by default. `readOnly` still blocks input locally.
- Phase-1 bug fixed: the seed reset mouse modes 1002/1003 after setting 1000, and libvterm turns
  tracking off on ANY of those resets. The seed now resets first and then sets only the mode that
  is on (likewise for the 1005/1006 encodings). Before the fix, a reattached vim/less lost mouse
  reporting until it set it again.
- `solidterm --help` prints usage (including `--tmux`) and exits before any Qt, window or
  single-instance code.
- `SOLIDTERM_TMUX_TRACE=1` prints every command line sent to tmux on stderr.

Tests: `solidterm-tmux-keys` checks the key table. `solidterm-tmux-input` runs the real UI against
the fake and checks the exact command lines for a key corpus (names × modifiers, coalesced
literals, quoting, an IME commit), a 100 KB bracketed paste in ≤ 1 KiB `-H` chunks, SGR click and
wheel, focus out/in reports, OSC 10/11 reports, no answers to DA/DSR/DECRQM/OSC queries in the
output, and lazy `select-pane`. `solidterm-tmux-input-real` uses a real tmux on a private socket:
typed `echo typed-$((6*7))` comes back as `typed-42`; `tests/tmux/mouse-echo.py`, a program like
vim or less (alternate screen, SGR mouse, bracketed paste), receives the click, the wheel and a
bracketed paste. `solidterm-help` covers `--help`.
