import { createSignal } from "solid-js";

export function InputDemo() {
  const [s, setS] = createSignal("");
  return (
    <div class="wrap">
      <input
        class="search"
        value={s()}
        onInput={(e) => setS(e.currentTarget.value)}
        placeholder="search…"
      />
    </div>
  );
}
