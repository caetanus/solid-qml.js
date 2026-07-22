// solidterm UI v1 — one live pane inside the embossed chrome; header shows the shell's OSC
// title; status bar shows session state. Splits/tabs/config are the next phases.
import { createSignal } from "solid-js";
import { TerminalView } from "qml:SolidTerm";
import "./term.css";

export function Term() {
  const [title, setTitle] = createSignal("solidterm");
  const [state, setState] = createSignal("live");
  return (
    <div class="term-root">
      <div class="term-header">
        <text class="term-title">{title()}</text>
        <text class="term-badge">v0.1 — pane 1</text>
      </div>
      <div class="term-body">
        <TerminalView
          class="term-pane"
          onTitleChanged={() => 0}
          onSessionFinished={() => setState("shell exited")}
        />
      </div>
      <div class="term-status">
        <text class="term-status-t">{state()}</text>
        <text class="term-hint">Ctrl+Shift+V paste · wheel scrollback</text>
      </div>
    </div>
  );
}
