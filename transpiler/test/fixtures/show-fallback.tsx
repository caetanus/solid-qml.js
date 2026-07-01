import { createSignal, Show } from "solid-js";

export function ShowFallback() {
  const [ok, setOk] = createSignal(true);
  return (
    <div class="wrap">
      <Show when={ok()} fallback={<text>none</text>}>
        <text>yes</text>
      </Show>
    </div>
  );
}
