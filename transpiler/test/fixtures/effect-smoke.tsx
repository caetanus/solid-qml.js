import { createSignal, createEffect, onCleanup } from "solid-js";
import { div, text } from "../../src/solid-qml/runtime";
export function EffectSmoke() {
  const [a, setA] = createSignal(1);
  const [b, setB] = createSignal(0);
  createEffect(() => setB(a() * 2));
  const t = setInterval(() => setA(a() + 1), 1000);
  onCleanup(() => clearInterval(t));
  return <div class="app"><text>a={a()} b={b()}</text></div>;
}
