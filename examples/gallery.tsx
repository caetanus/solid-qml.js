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
import { CssGaps } from "./cssgaps";
import { Dashboard } from "./dashboard";
import { Widgets } from "./widgets";
import { NativeOnly } from "./native";
import { ChartsAnd3D } from "./native/dataviz";
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
      <ToolBar class="nav">
        <ToolButton classList={{ active: view() === "hello" }} onClick={() => setView("hello")}>Hello</ToolButton>
        <ToolButton classList={{ active: view() === "counters" }} onClick={() => setView("counters")}>Counters</ToolButton>
        <ToolButton classList={{ active: view() === "counter" }} onClick={() => setView("counter")}>Counter (timer)</ToolButton>
        <ToolButton classList={{ active: view() === "children" }} onClick={() => setView("children")}>props.children</ToolButton>
        <ToolButton classList={{ active: view() === "derived" }} onClick={() => setView("derived")}>Derived</ToolButton>
        <ToolButton classList={{ active: view() === "index" }} onClick={() => setView("index")}>Index</ToolButton>
        <ToolButton classList={{ active: view() === "dynamic" }} onClick={() => setView("dynamic")}>Dynamic</ToolButton>
        <ToolButton classList={{ active: view() === "props" }} onClick={() => setView("props")}>Prop helpers</ToolButton>
        <ToolButton classList={{ active: view() === "context" }} onClick={() => setView("context")}>Context</ToolButton>
        <ToolButton classList={{ active: view() === "refs" }} onClick={() => setView("refs")}>Refs</ToolButton>
        <ToolButton classList={{ active: view() === "fetch" }} onClick={() => setView("fetch")}>Fetch</ToolButton>
        <ToolButton classList={{ active: view() === "todomvc" }} onClick={() => setView("todomvc")}>TodoMVC</ToolButton>
        <ToolButton classList={{ active: view() === "nodeimports" }} onClick={() => setView("nodeimports")}>Node imports</ToolButton>
        <ToolButton classList={{ active: view() === "cssgaps" }} onClick={() => setView("cssgaps")}>CSS gaps</ToolButton>
        <ToolButton classList={{ active: view() === "dashboard" }} onClick={() => setView("dashboard")}>Dashboard</ToolButton>
        <ToolButton classList={{ active: view() === "widgets" }} onClick={() => setView("widgets")}>Widgets</ToolButton>
        <ToolButton classList={{ active: view() === "native" }} onClick={() => setView("native")}>Native</ToolButton>
        <ToolButton classList={{ active: view() === "charts3d" }} onClick={() => setView("charts3d")}>Charts &amp; 3D</ToolButton>
      </ToolBar>
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
        <Match when={view() === "cssgaps"}><CssGaps /></Match>
        <Match when={view() === "dashboard"}><Dashboard /></Match>
        <Match when={view() === "widgets"}><Widgets /></Match>
        <Match when={view() === "native"}><NativeOnly /></Match>
        <Match when={view() === "charts3d"}><ChartsAnd3D /></Match>
      </Switch>
    </div>
  );
}
