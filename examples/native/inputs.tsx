// Extra native inputs (inputs group of the native-only showcase): RangeSlider, Dial,
// Tumbler, DelayButton, BusyIndicator, Round/Tool buttons and a ToolSeparator.
// Every capitalized tag below is QML-only surface resolved by the native-tag registry
// (no runtime import needed — the transpiler shadows them before user components).
import { createSignal } from "solid-js";
import { div, text, button } from "../../src/solid-qml/runtime";
import "./inputs.css";

export function ExtraInputs() {
  const [lo, setLo] = createSignal(20);
  const [hi, setHi] = createSignal(80);
  const [angle, setAngle] = createSignal(30);
  const [size, setSize] = createSignal("M");
  const [armed, setArmed] = createSignal(false);
  const [busy, setBusy] = createSignal(true);
  const [clicks, setClicks] = createSignal(0);

  return (
    <div class="nv-section">
      <text class="nv-title">Extra inputs</text>

      <div class="nv-row">
        <text class="nv-label">range {lo()}–{hi()}</text>
        <RangeSlider class="ni-range" min={0} max={100} step={1} first={lo()} second={hi()}
          onChange={(a, b) => { setLo(a); setHi(b); }} />
      </div>

      <div class="nv-row">
        <text class="nv-label">dial {angle()}</text>
        <Dial class="ni-dial" value={angle()} min={0} max={100} step={5} onChange={(v) => setAngle(v)} />
      </div>

      <div class="nv-row">
        <text class="nv-label">size {size()}</text>
        <Tumbler class="ni-tumbler" options={["S", "M", "L", "XL"]} value={size()} onChange={(v) => setSize(v)} />
      </div>

      <div class="nv-row">
        <DelayButton class="ni-delay" delay={1200} onActivated={() => setArmed(true)}>Hold to arm</DelayButton>
        <text class="nv-label">{armed() ? "armed!" : "idle"}</text>
      </div>

      <div class="nv-row">
        <button class="ni-btn" onClick={() => setBusy(!busy())}>{busy() ? "stop" : "start"}</button>
        <BusyIndicator class="ni-busy" running={busy()} />
      </div>

      <div class="nv-row">
        <RoundButton class="ni-round" onClick={() => setClicks(clicks() + 1)}>+1</RoundButton>
        <ToolSeparator class="ni-sep" />
        <ToolButton class="ni-tool" onClick={() => setClicks(0)}>reset</ToolButton>
        <text class="nv-label">clicks {clicks()}</text>
      </div>
    </div>
  );
}
