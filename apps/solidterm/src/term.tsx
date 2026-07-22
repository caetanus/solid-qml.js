// solidterm UI — tiling panes + right-click menu + a GNOME-style preferences window with a
// user-rebindable Keyboard section. All config (prefs AND keybindings) persists to
// ~/.config/solidterm/config.json through the native `termConfig` store.
import { createSignal, For } from "solid-js";
import { TerminalPanes, KeyRecorder } from "qml:SolidTerm";
import "./term.css";

declare const sysTheme: any;
declare const termConfig: any;
declare const Qt: any;
declare const process: any;

const SCHEMES: Record<string, { bg: string; fg: string }> = {
  midnight: { bg: "#161a21", fg: "#d4dae3" },
  solarized: { bg: "#002b36", fg: "#93a1a1" },
  gruvbox: { bg: "#282828", fg: "#ebdbb2" },
  paper: { bg: "#f7f2e9", fg: "#3a3532" },
};

// Actions the user can rebind. `def` is the tilix-flavoured default.
const ACTIONS = [
  { key: "splitRight", label: "Split right", def: "Ctrl+Shift+E" },
  { key: "splitDown", label: "Split down", def: "Ctrl+Shift+O" },
  { key: "closePane", label: "Close pane", def: "Ctrl+Shift+W" },
  { key: "focusNext", label: "Focus next pane", def: "Alt+Right" },
  { key: "focusPrev", label: "Focus previous pane", def: "Alt+Left" },
];

export function Term() {
  const [uiFontFamily, setUiFontFamily] = createSignal(termConfig.getString("fontFamily", "monospace"));
  const [uiFontSize, setUiFontSize] = createSignal(termConfig.getInt("fontSize", 15));
  const [scheme, setScheme] = createSignal(termConfig.getString("scheme", "system"));
  const [scrollback, setScrollback] = createSignal(termConfig.getInt("scrollback", 8000));
  const [cfgOpen, setCfgOpen] = createSignal(false);
  const [title, setTitle] = createSignal("solidterm");
  let term: any;

  // Keybindings: one signal per accelerator action, seeded from config (default when unset). The
  // <Shortcut> keys bind to these, so a rebind re-registers the accelerator live.
  const [kSplitRight, setKSplitRight] = createSignal(termConfig.getString("keys.splitRight", "Ctrl+Shift+E"));
  const [kSplitDown, setKSplitDown] = createSignal(termConfig.getString("keys.splitDown", "Ctrl+Shift+O"));
  const [kClosePane, setKClosePane] = createSignal(termConfig.getString("keys.closePane", "Ctrl+Shift+W"));
  const [kFocusNext, setKFocusNext] = createSignal(termConfig.getString("keys.focusNext", "Alt+Right"));
  const [kFocusPrev, setKFocusPrev] = createSignal(termConfig.getString("keys.focusPrev", "Alt+Left"));
  const getKey = (k: string) =>
    k === "splitRight" ? kSplitRight() : k === "splitDown" ? kSplitDown() : k === "closePane" ? kClosePane()
    : k === "focusNext" ? kFocusNext() : k === "focusPrev" ? kFocusPrev()
    : termConfig.getString("keys." + k, "");
  const setKey = (k: string, seq: string) => {
    termConfig.set("keys." + k, seq);
    if (k === "splitRight") setKSplitRight(seq);
    else if (k === "splitDown") setKSplitDown(seq);
    else if (k === "closePane") setKClosePane(seq);
    else if (k === "focusNext") setKFocusNext(seq);
    else if (k === "focusPrev") setKFocusPrev(seq);
  };

  return (
    <div class="term-root">
      <Shortcut keys={kSplitRight()} onActivated={() => term.split(Qt.Horizontal)} />
      <Shortcut keys={kSplitDown()} onActivated={() => term.split(Qt.Vertical)} />
      <Shortcut keys={kClosePane()} onActivated={() => term.closeFocused()} />
      <Shortcut keys={kFocusNext()} onActivated={() => term.focusNext()} />
      <Shortcut keys={kFocusPrev()} onActivated={() => term.focusPrev()} />

      <div class="term-header">
        <text class="term-title">{title()}</text>
        <button class="term-gear" onClick={() => setCfgOpen(true)}>⚙</button>
      </div>

      <div class="term-body">
        <TerminalPanes
          ref={term}
          class="term-pane"
          fontFamily={uiFontFamily()}
          fontSize={uiFontSize()}
          background={scheme() === "system" ? sysTheme.base : (SCHEMES[scheme()] || SCHEMES.midnight).bg}
          foreground={scheme() === "system" ? sysTheme.text : (SCHEMES[scheme()] || SCHEMES.midnight).fg}
          scrollbackLimit={scrollback()}
          handleColor={sysTheme.window}
          onTitleChanged={(t) => setTitle(t)}
          onAllClosed={() => process.exit(0)}
        />
        <ContextMenu class="tmenu">
          <MenuItem onClick={() => term.copyFocused()}>&Copy</MenuItem>
          <MenuItem onClick={() => term.pasteFocused()}>&Paste</MenuItem>
          <MenuSeparator />
          <MenuItem onClick={() => term.split(Qt.Horizontal)}>Split &right</MenuItem>
          <MenuItem onClick={() => term.split(Qt.Vertical)}>Split &down</MenuItem>
          <MenuItem onClick={() => term.closeFocused()}>Close &pane</MenuItem>
          <MenuSeparator />
          <MenuItem onClick={() => term.clearFocused()}>Clear scrollback</MenuItem>
          <MenuItem onClick={() => setCfgOpen(true)}>Pre&ferences…</MenuItem>
        </ContextMenu>
      </div>

      <div class="term-status">
        <text class="term-status-t">solidterm</text>
        <text class="term-hint">right-click for actions · shortcuts in Preferences</text>
      </div>

      <dialog open={cfgOpen()} title="Preferences" class="cfg" onClose={() => setCfgOpen(false)}>
        <div class="cfg-body">
          <text class="cfg-group">Appearance</text>
          <div class="cfg-card">
            <div class="cfg-row">
              <text class="cfg-l">Font family</text>
              <input class="cfg-in" value={uiFontFamily()}
                     onInput={(e) => { setUiFontFamily(e.target.value); termConfig.set("fontFamily", e.target.value); }} />
            </div>
            <hr class="cfg-div" />
            <div class="cfg-row">
              <text class="cfg-l">Font size</text>
              <select class="cfg-sel" value={"" + uiFontSize()}
                      onChange={(v) => { setUiFontSize(parseInt(v)); termConfig.set("fontSize", parseInt(v)); }}>
                <option value="11">11 px</option>
                <option value="12">12 px</option>
                <option value="13">13 px</option>
                <option value="14">14 px</option>
                <option value="15">15 px</option>
                <option value="16">16 px</option>
                <option value="18">18 px</option>
                <option value="20">20 px</option>
                <option value="24">24 px</option>
              </select>
            </div>
            <hr class="cfg-div" />
            <div class="cfg-row">
              <text class="cfg-l">Color scheme</text>
              <select class="cfg-sel" value={scheme()}
                      onChange={(v) => { setScheme(v); termConfig.set("scheme", v); }}>
                <option value="system">System</option>
                <option value="midnight">Midnight</option>
                <option value="solarized">Solarized Dark</option>
                <option value="gruvbox">Gruvbox</option>
                <option value="paper">Paper (light)</option>
              </select>
            </div>
          </div>

          <text class="cfg-group">Behavior</text>
          <div class="cfg-card">
            <div class="cfg-row">
              <text class="cfg-l">Scrollback</text>
              <select class="cfg-sel" value={"" + scrollback()}
                      onChange={(v) => { setScrollback(parseInt(v)); termConfig.set("scrollback", parseInt(v)); }}>
                <option value="1000">1000 lines</option>
                <option value="5000">5000 lines</option>
                <option value="8000">8000 lines</option>
                <option value="20000">20000 lines</option>
                <option value="100000">100000 lines</option>
              </select>
            </div>
          </div>

          <text class="cfg-group">Keyboard</text>
          <div class="cfg-card">
            <For each={ACTIONS}>
              {(a) => (
                <div class="cfg-krow">
                  <text class="cfg-l">{a.label}</text>
                  <KeyRecorder
                    class="cfg-rec"
                    sequence={getKey(a.key)}
                    background={sysTheme.base}
                    foreground={sysTheme.text}
                    accent={sysTheme.accent}
                    border={sysTheme.window}
                    onSequenceChanged={(s) => setKey(a.key, s)}
                  />
                </div>
              )}
            </For>
          </div>
          <text class="cfg-path">Saved to {termConfig.path}</text>

          <div class="cfg-actions">
            <button class="cfg-close" type="submit" onClick={() => setCfgOpen(false)}>Done</button>
          </div>
        </div>
      </dialog>
    </div>
  );
}
