// CSS showcase — every gap we closed, rendered so a single frame proves it works (the harness:
// `--grab`). One row/section per feature group, visually distinct. Static features only (a frame
// grab can't show hover/transition).
import { div, text } from "../src/solid-qml/runtime";
import "./cssgaps.css";

export function CssGaps() {
  return (
    <div class="gaps">
      {/* z-index + position:absolute — 3 overlap; #3 on top by z-index */}
      <div class="stack">
        <div class="z z1">1</div>
        <div class="z z2">2</div>
        <div class="z z3">3</div>
      </div>

      {/* opacity, transform (rotate/scale), visibility:hidden (keeps space) vs display:none */}
      <div class="row">
        <div class="cell op">op .4</div>
        <div class="cell rot">rot</div>
        <div class="cell scl">scale</div>
        <div class="cell vis">vis</div>
        <div class="cell hid">hidden</div>
        <div class="cell none">none</div>
        <div class="cell clip">overflow<div class="spill">spill</div></div>
      </div>

      {/* flex order (visual C,A,B) + flex-basis (120/80/100) */}
      <div class="flex">
        <div class="fi a">A·1</div>
        <div class="fi b">B·2</div>
        <div class="fi c">C·0</div>
      </div>

      {/* flex-shrink — 3×220 in a 560 row must shrink to fit */}
      <div class="shrink">
        <div class="si">shrink 220</div>
        <div class="si">shrink 220</div>
        <div class="si">shrink 220</div>
      </div>

      {/* box-sizing — both boxes are width:120; border-box stays 120, content-box grows to 160 */}
      <div class="row">
        <div class="bs border">border-box 120</div>
        <div class="bs content">content-box →160</div>
      </div>

      {/* grid-template-areas — header spans top; side + main below */}
      <div class="grid">
        <div class="g head">header</div>
        <div class="g side">side</div>
        <div class="g main">main</div>
      </div>

      {/* text: max-width wrap, decoration, white-space:nowrap, ellipsis, transform, letter/word-spacing */}
      <div class="textbox">
        <text class="t wrap">max-width 260: this sentence must wrap onto a second line instead of overflowing</text>
        <text class="t underline">underline</text>
        <text class="t strike">line-through</text>
        <text class="t nowrap">white-space:nowrap keeps this on one clipped line no matter what</text>
        <text class="t ellipsis">text-overflow ellipsis truncates this long line with a trailing…</text>
        <text class="t upper">text-transform uppercase</text>
        <text class="t spaced">letter + word spacing</text>
      </div>
    </div>
  );
}
