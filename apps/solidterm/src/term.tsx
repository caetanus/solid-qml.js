// solidterm UI — tiling panes + right-click menu + a GNOME-style preferences window with a
// user-rebindable Keyboard section. All config (prefs AND keybindings) persists to
// ~/.config/solidterm/config.json through the native `termConfig` store.
import { createSignal, For, Index, Show } from "solid-js";
import { TerminalTabs, KeyRecorder } from "qml:SolidTerm";
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
  { key: "newTab", label: "New tab", def: "Ctrl+Shift+T" },
  { key: "nextTab", label: "Next tab", def: "Ctrl+PgDown" },
  { key: "prevTab", label: "Previous tab", def: "Ctrl+PgUp" },
  { key: "search", label: "Find", def: "Ctrl+Shift+F" },
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
  // Decorations: background image (path), window opacity (percent), engraved text.
  const [bgImage, setBgImage] = createSignal(termConfig.getString("bgImage", ""));
  const browseBg = () => { const p = termConfig.pickImage(); if (p) { setBgImage(p); termConfig.set("bgImage", p); } };
  const clearBg = () => { setBgImage(""); termConfig.set("bgImage", ""); };
  const [opacity, setOpacity] = createSignal(termConfig.getInt("opacity", 100));
  const [emboss, setEmboss] = createSignal(termConfig.getInt("emboss", 0));
  sysTheme.setUiOpacity(opacity() / 100); // apply saved translucency to the chrome at startup
  const [cfgOpen, setCfgOpen] = createSignal(false);
  const [title, setTitle] = createSignal("solidterm");
  // Scrollback search (Ctrl+Shift+F): the bar is Solid; the native pane does the find/highlight.
  const [searchOpen, setSearchOpen] = createSignal(false);
  const [searchQuery, setSearchQuery] = createSignal("");
  const [searchIdx, setSearchIdx] = createSignal(0);
  const [searchCount, setSearchCount] = createSignal(0);
  let searchInput: any;
  const openSearch = () => { setSearchOpen(true); if (searchInput) searchInput.forceActiveFocus(); };
  const closeSearch = () => { setSearchOpen(false); term.clearSearch(); term.refocus(); };
  // Multiline-paste confirmation (a stray newline would run the command).
  const [pastePrompt, setPastePrompt] = createSignal("");
  const pasteLineCount = () => { const p = pastePrompt(); return p ? p.split("\n").length : 0; };
  const confirmPaste = () => { term.pasteTextFocused(pastePrompt()); setPastePrompt(""); };
  // Editable tab title (overrides the OSC title; empty reverts to automatic).
  const [renameOpen, setRenameOpen] = createSignal(false);
  const [renameValue, setRenameValue] = createSignal("");
  const openRename = () => { setRenameValue(term.tabTitle(activeTab()) || ""); setRenameOpen(true); };
  const applyRename = () => { term.setTabTitle(activeTab(), renameValue()); setRenameOpen(false); term.refocus(); };
  // Tab model mirrored from the native (headless) stack: one title per tab + the active index. The
  // bar is rendered here in Solid; the native side just keeps the ptys alive and shows one by index.
  const [tabTitles, setTabTitles] = createSignal(["terminal"]);
  const [activeTab, setActiveTab] = createSignal(0);
  const tabLabel = (t: string) => {
    if (!t) return "terminal";
    let s = t;
    if (s.endsWith("/")) s = s.slice(0, -1); // drop a trailing slash
    const slash = s.lastIndexOf("/");         // show the last path segment
    return slash >= 0 ? s.slice(slash + 1) : s;
  };
  let term: any;

  // Keybindings: one signal per accelerator action, seeded from config (default when unset). The
  // <Shortcut> keys bind to these, so a rebind re-registers the accelerator live.
  const [kSplitRight, setKSplitRight] = createSignal(termConfig.getString("keys.splitRight", "Ctrl+Shift+E"));
  const [kSplitDown, setKSplitDown] = createSignal(termConfig.getString("keys.splitDown", "Ctrl+Shift+O"));
  const [kClosePane, setKClosePane] = createSignal(termConfig.getString("keys.closePane", "Ctrl+Shift+W"));
  const [kFocusNext, setKFocusNext] = createSignal(termConfig.getString("keys.focusNext", "Alt+Right"));
  const [kFocusPrev, setKFocusPrev] = createSignal(termConfig.getString("keys.focusPrev", "Alt+Left"));
  const [kNewTab, setKNewTab] = createSignal(termConfig.getString("keys.newTab", "Ctrl+Shift+T"));
  const [kNextTab, setKNextTab] = createSignal(termConfig.getString("keys.nextTab", "Ctrl+PgDown"));
  const [kPrevTab, setKPrevTab] = createSignal(termConfig.getString("keys.prevTab", "Ctrl+PgUp"));
  const [kSearch, setKSearch] = createSignal(termConfig.getString("keys.search", "Ctrl+Shift+F"));
  const getKey = (k: string) =>
    k === "splitRight" ? kSplitRight() : k === "splitDown" ? kSplitDown() : k === "closePane" ? kClosePane()
    : k === "focusNext" ? kFocusNext() : k === "focusPrev" ? kFocusPrev() : k === "newTab" ? kNewTab()
    : k === "nextTab" ? kNextTab() : k === "prevTab" ? kPrevTab() : k === "search" ? kSearch()
    : termConfig.getString("keys." + k, "");
  // Accelerators are consumed by the focused terminal (not Qt's ambiguous Shortcut map) and
  // dispatched here by matching the pressed sequence to the configured binding.
  const onAccel = (seq: string) => {
    if (seq === kSplitRight()) term.split(Qt.Horizontal);
    else if (seq === kSplitDown()) term.split(Qt.Vertical);
    else if (seq === kClosePane()) term.closeFocused();
    else if (seq === kFocusNext()) term.focusNext();
    else if (seq === kFocusPrev()) term.focusPrev();
    else if (seq === kNewTab()) term.newTab();
    else if (seq === kNextTab()) { const n = tabTitles().length; if (n > 1) term.selectTab((activeTab() + 1) % n); }
    else if (seq === kPrevTab()) { const n = tabTitles().length; if (n > 1) term.selectTab((activeTab() - 1 + n) % n); }
    else if (seq === kSearch()) openSearch();
    else if (seq === "Ctrl+=" || seq === "Ctrl++") zoomFont(1);
    else if (seq === "Ctrl+-") zoomFont(-1);
    else if (seq === "Ctrl+0") zoomFont(0);
  };
  // Font zoom (Ctrl +/−/0). 0 resets to the default.
  const zoomFont = (d: number) => {
    const n = d === 0 ? 15 : Math.max(6, Math.min(40, uiFontSize() + d));
    setUiFontSize(n);
    termConfig.set("fontSize", n);
  };
  const setKey = (k: string, seq: string) => {
    termConfig.set("keys." + k, seq);
    if (k === "splitRight") setKSplitRight(seq);
    else if (k === "splitDown") setKSplitDown(seq);
    else if (k === "closePane") setKClosePane(seq);
    else if (k === "focusNext") setKFocusNext(seq);
    else if (k === "focusPrev") setKFocusPrev(seq);
    else if (k === "newTab") setKNewTab(seq);
    else if (k === "nextTab") setKNextTab(seq);
    else if (k === "prevTab") setKPrevTab(seq);
    else if (k === "search") setKSearch(seq);
  };

  return (
    <div class="term-root">
      <div class="term-header">
        <text class="term-title">{title()}</text>
        <button class="term-gear" onClick={() => setCfgOpen(true)}>⚙</button>
      </div>

      <Show when={tabTitles().length > 1}>
        <div class="tabbar">
          <Index each={tabTitles()}>{(t, i) =>
            <div class="tab" classList={{ "tab-active": i === activeTab() }} onClick={() => term.selectTab(i)}>
              <text class="tab-label">{tabLabel(t())}</text>
              <button class="tab-x" onClick={() => term.closeTab(i)}>✕</button>
            </div>
          }</Index>
          <button class="tab-new" onClick={() => term.newTab()}>＋</button>
        </div>
      </Show>

      <Show when={searchOpen()}>
        <div class="searchbar">
          <input ref={searchInput} class="search-in" value={searchQuery()} placeholder="Find…"
                 onInput={(e) => { setSearchQuery(e.target.value); term.searchFocused(e.target.value); }} />
          <text class="search-count">{searchCount() > 0 ? (searchIdx() + " / " + searchCount()) : "0 / 0"}</text>
          <button class="search-btn" onClick={() => term.searchPrev()}>‹</button>
          <button class="search-btn" onClick={() => term.searchNext()}>›</button>
          <button class="search-btn" onClick={() => closeSearch()}>✕</button>
        </div>
      </Show>

      <div class="term-body">
        <TerminalTabs
          ref={term}
          class="term-pane"
          onTabsChanged={(titles, active) => { setTabTitles(titles); setActiveTab(active); }}
          onSearchChanged={(idx, count) => { setSearchIdx(idx); setSearchCount(count); }}
          onUnsafePasteRequested={(text) => setPastePrompt(text)}
          fontFamily={uiFontFamily()}
          fontSize={uiFontSize()}
          background={scheme() === "system" ? sysTheme.base : (SCHEMES[scheme()] || SCHEMES.midnight).bg}
          foreground={scheme() === "system" ? sysTheme.text : (SCHEMES[scheme()] || SCHEMES.midnight).fg}
          scrollbackLimit={scrollback()}
          backgroundImage={bgImage()}
          backgroundOpacity={opacity() / 100}
          emboss={emboss() !== 0}
          handleColor={sysTheme.window}
          reservedSequences={[kSplitRight(), kSplitDown(), kClosePane(), kFocusNext(), kFocusPrev(), kNewTab(), kNextTab(), kPrevTab(), kSearch(), "Ctrl+=", "Ctrl++", "Ctrl+-", "Ctrl+0"]}
          onAccelerator={(seq) => onAccel(seq)}
          onTitleChanged={(t) => setTitle(t)}
          onAllClosed={() => process.exit(0)}
        />
        <ContextMenu class="tmenu">
          <MenuItem onClick={() => term.copyFocused()}>&Copy</MenuItem>
          <MenuItem onClick={() => term.pasteFocused()}>&Paste</MenuItem>
          <MenuSeparator />
          <MenuItem onClick={() => term.newTab()}>New &tab</MenuItem>
          <MenuItem onClick={() => term.split(Qt.Horizontal)}>Split &right</MenuItem>
          <MenuItem onClick={() => term.split(Qt.Vertical)}>Split &down</MenuItem>
          <MenuItem onClick={() => term.closeFocused()}>Close &pane</MenuItem>
          <MenuSeparator />
          <MenuItem onClick={() => openSearch()}>&Find…</MenuItem>
          <MenuItem onClick={() => openRename()}>Set &title…</MenuItem>
          <MenuItem onClick={() => term.clearFocused()}>Clear scrollback</MenuItem>
          <MenuItem onClick={() => term.resetFocused()}>&Reset</MenuItem>
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
            <hr class="cfg-div" />
            <div class="cfg-row">
              <text class="cfg-l">Background image</text>
              <div class="cfg-imgrow">
                <input class="cfg-in-img" value={bgImage()} placeholder="none"
                       onInput={(e) => { setBgImage(e.target.value); termConfig.set("bgImage", e.target.value); }} />
                <button class="cfg-browse" onClick={() => browseBg()}>Browse…</button>
                <button class="cfg-browse" onClick={() => clearBg()}>✕</button>
              </div>
            </div>
            <hr class="cfg-div" />
            <div class="cfg-row">
              <text class="cfg-l">Opacity</text>
              <select class="cfg-sel" value={"" + opacity()}
                      onChange={(v) => { setOpacity(parseInt(v)); termConfig.set("opacity", parseInt(v)); sysTheme.setUiOpacity(parseInt(v) / 100); }}>
                <option value="100">100% (opaque)</option>
                <option value="95">95%</option>
                <option value="90">90%</option>
                <option value="85">85%</option>
                <option value="75">75%</option>
                <option value="65">65%</option>
                <option value="50">50%</option>
              </select>
            </div>
            <hr class="cfg-div" />
            <div class="cfg-row">
              <text class="cfg-l">Emboss text</text>
              <select class="cfg-sel" value={"" + emboss()}
                      onChange={(v) => { setEmboss(parseInt(v)); termConfig.set("emboss", parseInt(v)); }}>
                <option value="0">Off</option>
                <option value="1">On</option>
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

      <dialog open={renameOpen()} title="Rename tab" class="cfg" onClose={() => setRenameOpen(false)}>
        <div class="cfg-body paste-body">
          <text class="paste-warn">Tab title</text>
          <input class="cfg-in rename-in" value={renameValue()} placeholder="(empty = automatic)"
                 onInput={(e) => setRenameValue(e.target.value)} />
          <div class="cfg-actions paste-actions">
            <button class="paste-cancel" onClick={() => setRenameOpen(false)}>Cancel</button>
            <button class="cfg-close" type="submit" onClick={() => applyRename()}>Set</button>
          </div>
        </div>
      </dialog>

      <dialog open={pastePrompt() !== ""} title="Paste" class="cfg" onClose={() => setPastePrompt("")}>
        <div class="cfg-body paste-body">
          <text class="paste-warn">Paste {pasteLineCount()} lines into the terminal?</text>
          <text class="paste-hint">Multi-line paste can run commands. Review before confirming.</text>
          <div class="cfg-actions paste-actions">
            <button class="paste-cancel" onClick={() => setPastePrompt("")}>Cancel</button>
            <button class="cfg-close" type="submit" onClick={() => confirmPaste()}>Paste</button>
          </div>
        </div>
      </dialog>
    </div>
  );
}
