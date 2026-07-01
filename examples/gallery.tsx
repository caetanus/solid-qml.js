// The example launcher as a plain Solid component — navigates between example components with a
// signal + <Switch> (no iframe, no C++). Same component for web (main.tsx) and native (mainqml.tsx);
// only the entry differs. The transpiler follows these imports to emit one QML per component.
import { createSignal, Switch, Match, Show } from "solid-js";
import { div, text, button } from "../src/solid-qml/runtime";
import { Hello } from "./hello";
import { Counters } from "./counters";
import { TimerCounter } from "./counter";
import { Children } from "./children";
import { Derived } from "./derived";
import { IndexList } from "./index-list";
import { DynamicTag } from "./dynamic";
import { PropHelpers } from "./props-helpers";
import { ContextDemo } from "./context";
import { Refs } from "./refs";
import { Fetch } from "./fetch";
import { TodoMVC } from "./todomvc";
import { NodeImports } from "./nodeimports";
import "./examples.css";

export function Gallery() {
  const [view, setView] = createSignal("hello");

  return (
    <div class="gallery">
      {/* Hero gradient scoped to the Hello view — a full-bleed layer behind the (transparent) nav
          and content, so only Hello gets the gradient while the other examples keep the flat theme. */}
      <Show when={view() === "hello"}>
        <div class="hero-bg" />
      </Show>
      <div class="nav">
        <button onClick={() => setView("hello")}>Hello</button>
        <button onClick={() => setView("counters")}>Counters</button>
        <button onClick={() => setView("counter")}>Counter (timer)</button>
        <button onClick={() => setView("children")}>props.children</button>
        <button onClick={() => setView("derived")}>Derived</button>
        <button onClick={() => setView("index")}>Index</button>
        <button onClick={() => setView("dynamic")}>Dynamic</button>
        <button onClick={() => setView("props")}>Prop helpers</button>
        <button onClick={() => setView("context")}>Context</button>
        <button onClick={() => setView("refs")}>Refs</button>
        <button onClick={() => setView("fetch")}>Fetch</button>
        <button onClick={() => setView("todomvc")}>TodoMVC</button>
        <button onClick={() => setView("nodeimports")}>Node imports</button>
      </div>
      <Switch>
        <Match when={view() === "hello"}><Hello /></Match>
        <Match when={view() === "counters"}><Counters /></Match>
        <Match when={view() === "counter"}><TimerCounter /></Match>
        <Match when={view() === "children"}><Children /></Match>
        <Match when={view() === "derived"}><Derived /></Match>
        <Match when={view() === "index"}><IndexList /></Match>
        <Match when={view() === "dynamic"}><DynamicTag /></Match>
        <Match when={view() === "props"}><PropHelpers /></Match>
        <Match when={view() === "context"}><ContextDemo /></Match>
        <Match when={view() === "refs"}><Refs /></Match>
        <Match when={view() === "fetch"}><Fetch /></Match>
        <Match when={view() === "todomvc"}><TodoMVC /></Match>
        <Match when={view() === "nodeimports"}><NodeImports /></Match>
      </Switch>
    </div>
  );
}
