// One source, two targets: `npm run dev` renders this in the browser (Vite + Solid);
// `npx solid-qml dev` / `build` transpile it to QML and render it natively with Qt Quick.
import { createSignal } from "solid-js";
import { render } from "solid-js/web";
import { Window } from "solid-qml";
import "./app.css";

function Counter() {
  const [count, setCount] = createSignal(0);
  return (
    <div class="counter">
      <text class="count">{count()}</text>
      <div class="row">
        <button class="secondary" onClick={() => setCount(count() - 1)}>-</button>
        <button onClick={() => setCount(count() + 1)}>+1</button>
      </div>
    </div>
  );
}

export function App() {
  return (
    <Window title="__PRODUCT__" width={420} height={320} visible>
      <div class="app">
        <text class="title">__PRODUCT__</text>
        <text class="subtitle">Solid.js, rendered natively with Qt Quick</text>
        <Counter />
      </div>
    </Window>
  );
}

render(() => <App />, document.getElementById("root")!);
