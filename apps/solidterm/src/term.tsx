// solidterm UI — pane + right-click menu + preferences dialog. The config persists in
// localStorage (the shim's sqlite store) and applies live through TerminalView props.
import { createSignal } from "solid-js";
import { TerminalView } from "qml:SolidTerm";
import "./term.css";

declare const process: any;

const SCHEMES: Record<string, { bg: string; fg: string; label: string }> = {
  midnight: { bg: "#161a21", fg: "#d4dae3", label: "Midnight" },
  solarized: { bg: "#002b36", fg: "#93a1a1", label: "Solarized Dark" },
  gruvbox: { bg: "#282828", fg: "#ebdbb2", label: "Gruvbox" },
  paper: { bg: "#f7f2e9", fg: "#3a3532", label: "Paper (light)" },
};

export function Term() {
  // Config load inlined per-initializer: module consts support literals only (subset), and the
  // localStorage shim is live before any binding evaluates.
  const [uiFontFamily, setUiFontFamily] = createSignal(JSON.parse(localStorage.getItem("solidterm.cfg") || "{}").fontFamily || "monospace");
  const [uiFontSize, setUiFontSize] = createSignal(JSON.parse(localStorage.getItem("solidterm.cfg") || "{}").fontSize || 15);
  const [scheme, setScheme] = createSignal(JSON.parse(localStorage.getItem("solidterm.cfg") || "{}").scheme || "midnight");
  const [scrollback, setScrollback] = createSignal(JSON.parse(localStorage.getItem("solidterm.cfg") || "{}").scrollback || 8000);
  const [cfgOpen, setCfgOpen] = createSignal(false);
  const [state, setState] = createSignal("live");
  let term: any;

  const save = () =>
    localStorage.setItem("solidterm.cfg", JSON.stringify({
      fontFamily: uiFontFamily(), fontSize: uiFontSize(), scheme: scheme(), scrollback: scrollback(),
    }));

  return (
    <div class="term-root">
      <div class="term-header">
        <text class="term-title">solidterm</text>
        <button class="term-gear" onClick={() => setCfgOpen(true)}>⚙</button>
      </div>
      <div class="term-body">
        <TerminalView
          ref={term}
          class="term-pane"
          fontFamily={uiFontFamily()}
          fontSize={uiFontSize()}
          background={(SCHEMES[scheme()] || SCHEMES.midnight).bg}
          foreground={(SCHEMES[scheme()] || SCHEMES.midnight).fg}
          scrollbackLimit={scrollback()}
          onSessionFinished={() => process.exit(0)}
        />
        <ContextMenu>
          <MenuItem onClick={() => term.copySelection()}>&Copy</MenuItem>
          <MenuItem onClick={() => term.pasteClipboard()}>&Paste</MenuItem>
          <MenuSeparator />
          <MenuItem onClick={() => term.clearScrollback()}>Clear scrollback</MenuItem>
          <MenuSeparator />
          <MenuItem onClick={() => setCfgOpen(true)}>Pre&ferences…</MenuItem>
        </ContextMenu>
      </div>
      <div class="term-status">
        <text class="term-status-t">{state()}</text>
        <text class="term-hint">Ctrl+Shift+C/V copy/paste · wheel scrollback · right-click menu</text>
      </div>

      <dialog open={cfgOpen()} title="solidterm — preferences" class="cfg" onClose={() => setCfgOpen(false)}>
        <div class="cfg-grid">
          <text class="cfg-l">Font family</text>
          <input value={uiFontFamily()} onInput={(e) => { setUiFontFamily(e.target.value); save(); }} />
          <text class="cfg-l">Font size</text>
          <input type="number" min={8} max={32} value={uiFontSize()}
                 onChange={(v) => { setUiFontSize(v); save(); }} />
          <text class="cfg-l">Theme</text>
          <select value={scheme()} onChange={(v) => { setScheme(v); save(); }}>
            <option value="midnight">Midnight</option>
            <option value="solarized">Solarized Dark</option>
            <option value="gruvbox">Gruvbox</option>
            <option value="paper">Paper (light)</option>
          </select>
          <text class="cfg-l">Scrollback lines</text>
          <input type="number" min={0} max={100000} step={1000} value={scrollback()}
                 onChange={(v) => { setScrollback(v); save(); }} />
        </div>
        <div class="cfg-actions">
          <button type="submit" onClick={() => setCfgOpen(false)}>Close</button>
        </div>
      </dialog>
    </div>
  );
}
