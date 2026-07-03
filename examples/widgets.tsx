// Widgets gallery — every native form control (Phase 6 of the native-widgets plan).
// One page per control family, values wired to signals; the "Form state" card at the
// bottom echoes all values reactively and provides a Reset button.
import { createSignal } from "solid-js";
import { tabstop } from "qml-solid";
import { div, text, button } from "../src/solid-qml/runtime";
import "./widgets.css";

export function Widgets() {
  // ── text section ───────────────────────────────────────────────────────────────
  const [inputName, setInputName] = createSignal("");
  const [inputPass, setInputPass] = createSignal("");
  const [inputSearch, setInputSearch] = createSignal("");
  const [inputNotes, setInputNotes] = createSignal("");

  // ── toggles ────────────────────────────────────────────────────────────────────
  const [chk1, setChk1] = createSignal(false);
  const [chk2, setChk2] = createSignal(true);
  const [sw, setSw] = createSignal(false);
  // Live tabstop opt-out (qml-solid runtime API); flipping it re-evaluates every widget's tab stop.
  const [tabs, setTabs] = createSignal(true);
  // Single plan signal — each radio's onChange sets the string value.
  const [plan, setPlan] = createSignal("free");

  // ── pickers ────────────────────────────────────────────────────────────────────
  const [fruit, setFruit] = createSignal("apple");
  const [volume, setVolume] = createSignal(50);
  const [qty, setQty] = createSignal(1);
  const [inputDate, setInputDate] = createSignal(null);
  const [calDate, setCalDate] = createSignal(null);

  // ── reset ─────────────────────────────────────────────────────────────────────
  function reset() {
    setInputName("");
    setInputPass("");
    setInputSearch("");
    setInputNotes("");
    setChk1(false);
    setChk2(true);
    setSw(false);
    setPlan("free");
    setFruit("apple");
    setVolume(50);
    setQty(1);
    setInputDate(null);
    setCalDate(null);
  }

  return (
    <div class="widgets">
      <div class="wg-cols">
        {/* ── Text ───────────────────────────────────────────────── */}
        <div class="wg-section">
          <text class="wg-title">Text</text>

          <div class="wg-field">
            <text class="wg-label">Name</text>
            <input class="wg-input" placeholder="Your name" value={inputName()} onInput={(e) => setInputName(e.target.value)} />
          </div>

          <div class="wg-field">
            <text class="wg-label">Password</text>
            <input class="wg-input" type="password" placeholder="Secret" value={inputPass()} onInput={(e) => setInputPass(e.target.value)} />
          </div>

          <div class="wg-field">
            <text class="wg-label">Search (max 20)</text>
            <input class="wg-input" type="text" placeholder="Search…" maxlength={20} value={inputSearch()} onInput={(e) => setInputSearch(e.target.value)} />
          </div>

          <div class="wg-field">
            <text class="wg-label">Disabled</text>
            <input class="wg-input" placeholder="Not editable" disabled />
          </div>

          <div class="wg-field">
            <text class="wg-label">Notes</text>
            <textarea class="wg-textarea" placeholder="Add notes…" value={inputNotes()} onInput={(e) => setInputNotes(e.target.value)}></textarea>
          </div>
        </div>

        {/* ── Toggles ────────────────────────────────────────────── */}
        <div class="wg-section">
          <text class="wg-title">Toggles</text>

          <div class="wg-check-row">
            <input type="checkbox" class="wg-checkbox" checked={chk1()} onChange={(e) => setChk1(e.target.checked)} />
            <text class="wg-check-label">Accept terms</text>
          </div>

          <div class="wg-check-row">
            <input type="checkbox" class="wg-checkbox" checked={chk2()} onChange={(e) => setChk2(e.target.checked)} />
            <text class="wg-check-label">Send updates (pre-checked)</text>
          </div>

          <div class="wg-check-row">
            <input type="checkbox" role="switch" class="wg-switch" checked={sw()} onChange={(e) => setSw(e.target.checked)} />
            <text class="wg-check-label">Dark mode</text>
          </div>

          <div class="wg-check-row">
            <input type="checkbox" class="wg-checkbox" disabled />
            <text class="wg-check-label">Disabled checkbox</text>
          </div>

          <div class="wg-check-row">
            <input type="checkbox" role="switch" class="wg-switch" checked={tabs()}
                   onChange={(e) => { setTabs(e.target.checked); tabstop.enabled = e.target.checked; }} />
            <text class="wg-check-label">Tab navigation (native only)</text>
          </div>

          <text class="wg-sublabel">Plan</text>

          <div class="wg-check-row">
            <input type="radio" name="plan" class="wg-radio" checked={plan() === "free"} onChange={(e) => setPlan("free")} />
            <text class="wg-check-label">Free</text>
          </div>

          <div class="wg-check-row">
            <input type="radio" name="plan" class="wg-radio" checked={plan() === "pro"} onChange={(e) => setPlan("pro")} />
            <text class="wg-check-label">Pro</text>
          </div>

          <div class="wg-check-row">
            <input type="radio" name="plan" class="wg-radio" checked={plan() === "team"} onChange={(e) => setPlan("team")} />
            <text class="wg-check-label">Team</text>
          </div>
        </div>
      </div>

      <div class="wg-cols">
        {/* ── Pickers ────────────────────────────────────────────── */}
        <div class="wg-section">
          <text class="wg-title">Pickers</text>

          <div class="wg-field">
            <text class="wg-label">Fruit</text>
            <select class="wg-select" value={fruit()} onChange={(e) => setFruit(e.target.value)}>
              <option value="apple">Apple</option>
              <option value="banana">Banana</option>
              <option value="cherry">Cherry</option>
              <option value="mango">Mango</option>
            </select>
          </div>

          <div class="wg-field">
            <div class="wg-range-header">
              <text class="wg-label">Volume</text>
              <text class="wg-range-val">{volume()}</text>
            </div>
            <input type="range" class="wg-range" min={0} max={100} step={1} value={volume()} onInput={(e) => setVolume(e.target.value)} />
          </div>

          <div class="wg-field">
            <text class="wg-label">Quantity</text>
            <input type="number" class="wg-number" min={1} max={10} step={1} value={qty()} onChange={(e) => setQty(e.target.value)} />
          </div>

          <div class="wg-field">
            <text class="wg-label">Date</text>
            <input type="date" class="wg-date" value={inputDate()} onChange={(e) => setInputDate(e.target.value)} />
          </div>
        </div>

        {/* ── Calendar ────────────────────────────────────────────── */}
        <div class="wg-section wg-cal-section">
          <text class="wg-title">Calendar</text>
          <Calendar class="wg-cal" value={calDate()} onChange={(e) => setCalDate(e.target.value)} />
        </div>
      </div>

      {/* ── Form state ──────────────────────────────────────────────────────── */}
      <div class="wg-summary">
        <text class="wg-title">Form state</text>
        <div class="wg-kv-grid">
          <div class="wg-kv-row">
            <text class="wg-kv">name</text>
            <text class="wg-kv-v">{inputName()}</text>
          </div>
          <div class="wg-kv-row">
            <text class="wg-kv">pass</text>
            <text class="wg-kv-v">{inputPass() ? "set" : "—"}</text>
          </div>
          <div class="wg-kv-row">
            <text class="wg-kv">search</text>
            <text class="wg-kv-v">{inputSearch()}</text>
          </div>
          <div class="wg-kv-row">
            <text class="wg-kv">notes</text>
            <text class="wg-kv-v">{inputNotes() ? "set" : "—"}</text>
          </div>
          <div class="wg-kv-row">
            <text class="wg-kv">check1</text>
            <text class="wg-kv-v">{chk1() ? "on" : "off"}</text>
          </div>
          <div class="wg-kv-row">
            <text class="wg-kv">check2</text>
            <text class="wg-kv-v">{chk2() ? "on" : "off"}</text>
          </div>
          <div class="wg-kv-row">
            <text class="wg-kv">switch</text>
            <text class="wg-kv-v">{sw() ? "on" : "off"}</text>
          </div>
          <div class="wg-kv-row">
            <text class="wg-kv">plan</text>
            <text class="wg-kv-v">{plan()}</text>
          </div>
          <div class="wg-kv-row">
            <text class="wg-kv">fruit</text>
            <text class="wg-kv-v">{fruit()}</text>
          </div>
          <div class="wg-kv-row">
            <text class="wg-kv">volume</text>
            <text class="wg-kv-v">{volume()}</text>
          </div>
          <div class="wg-kv-row">
            <text class="wg-kv">qty</text>
            <text class="wg-kv-v">{qty()}</text>
          </div>
          <div class="wg-kv-row">
            <text class="wg-kv">date</text>
            <text class="wg-kv-v">{inputDate() ? "selected" : "—"}</text>
          </div>
          <div class="wg-kv-row">
            <text class="wg-kv">cal</text>
            <text class="wg-kv-v">{calDate() ? "selected" : "—"}</text>
          </div>
        </div>
        <button class="wg-reset" onClick={() => reset()}>Reset</button>
      </div>
    </div>
  );
}
