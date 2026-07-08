import { createSignal } from "solid-js";
import type { JSX } from "solid-js";
// Normalise UA chrome, then apply the shared base defaults, so the browser preview matches the
// native QML render (which loads the same base layer) and the shared stylesheet is authoritative.
import "./reset.css";
import "./base.css";

type WindowProps = {
  title?: string;
  width?: number;
  height?: number;
  visible?: boolean;
  class?: string;
  className?: string;
  children?: JSX.Element;
};

type PrimitiveProps = {
  id?: string;
  class?: string;
  className?: string;
  children?: JSX.Element;
  [key: string]: unknown;
};

function classes(props: PrimitiveProps, primitive: string) {
  return ["qml", `qml-${primitive}`, props.class, props.className]
    .filter(Boolean)
    .join(" ");
}

export function Window(props: WindowProps) {
  return (
    <main
      class={["qml-window", props.class, props.className].filter(Boolean).join(" ")}
      data-qml-type="Window"
      data-title={props.title}
      style={{
        "--window-width": props.width ? `${props.width}px` : undefined,
        "--window-height": props.height ? `${props.height}px` : undefined,
        display: props.visible === false ? "none" : undefined,
      }}
    >
      {props.children}
    </main>
  );
}

export function div(props: PrimitiveProps) {
  return (
    <div id={props.id} class={classes(props, "div")} data-qml-type="CssRect">
      {props.children}
    </div>
  );
}

export function text(props: PrimitiveProps) {
  return (
    <span id={props.id} class={classes(props, "text")} data-qml-type="CssText">
      {props.children}
    </span>
  );
}

export function span(props: PrimitiveProps) {
  return text({ ...props, class: [props.class, "inline"].filter(Boolean).join(" ") });
}

export function button(props: PrimitiveProps) {
  const onClick = props.onClick ?? props.onClicked;

  return (
    <button
      id={props.id}
      class={classes(props, "button")}
      data-qml-type="Button"
      type="button"
      onClick={onClick as JSX.EventHandlerUnion<HTMLButtonElement, MouseEvent>}
    >
      {props.children}
    </button>
  );
}

// <Calendar> — the native target's inline month grid, mirrored for the browser preview so
// the shared source renders on both targets. Same class surface as the native emission
// (.cal-nav/.cal-title/.dow/.day/.day-label with selected/today/outside states), so the
// factory sheet and authored rules style both identically.
export function Calendar(props: {
  value?: Date | null;
  onChange?: (e: { target: { value: Date; valueAsDate: Date } }) => void;
  class?: string;
  className?: string;
}) {
  const [shown, setShown] = createSignal(
    props.value instanceof Date ? new Date(props.value.getFullYear(), props.value.getMonth(), 1) : new Date(),
  );
  const nav = (d: number) => setShown((m) => new Date(m.getFullYear(), m.getMonth() + d, 1));
  const cells = () => {
    const m = shown();
    const first = new Date(m.getFullYear(), m.getMonth(), 1);
    const start = new Date(first.getFullYear(), first.getMonth(), 1 - first.getDay());
    return Array.from({ length: 42 }, (_, i) =>
      new Date(start.getFullYear(), start.getMonth(), start.getDate() + i));
  };
  const same = (a: Date | null | undefined, b: Date) =>
    a instanceof Date && a.getFullYear() === b.getFullYear() && a.getMonth() === b.getMonth() && a.getDate() === b.getDate();
  const dows = ["dom.", "seg.", "ter.", "qua.", "qui.", "sex.", "sáb."];
  return (
    <div class={["qml", props.class, props.className].filter(Boolean).join(" ")} data-qml-type="Calendar"
         style={{ position: "relative" }}>
      <div style={{ display: "flex", "align-items": "center" }}>
        <button class="cal-nav" type="button" onClick={() => nav(-1)}>‹</button>
        <span class="cal-title" style={{ flex: "1", "text-align": "center" }}>
          {shown().toLocaleDateString(undefined, { month: "long", year: "numeric" })}
        </span>
        <button class="cal-nav" type="button" onClick={() => nav(1)}>›</button>
      </div>
      <div style={{ display: "grid", "grid-template-columns": "repeat(7, 1fr)" }}>
        {dows.map((d) => <span class="dow">{d}</span>)}
        {cells().map((d) => {
          const today = same(new Date(), d);
          const outside = () => d.getMonth() !== shown().getMonth();
          const selected = () => same(props.value ?? null, d);
          return (
            <button type="button"
              class={["day", selected() ? "selected" : "", today ? "today" : "", outside() ? "outside" : ""].filter(Boolean).join(" ")}
              style={{ border: "none", background: "transparent", padding: "0" }}
              onClick={() => props.onChange?.({ target: { value: d, valueAsDate: d } })}>
              <span class={["day-label", selected() ? "selected" : "", outside() ? "outside" : ""].filter(Boolean).join(" ")}>
                {d.getDate()}
              </span>
            </button>
          );
        })}
      </div>
    </div>
  );
}

// Native tab-navigation switch (`import { tabstop } from "qml-solid"`). In the QML target the
// transpiler rewrites this to the loader's reactive `solidTabstop` context property; here it
// only mirrors the API so shared sources typecheck — the browser keeps the DOM's own tab order.
export const tabstop = { enabled: true };

// Desktop notifications (`import { notifications } from "qml-solid"`). In the QML target the
// transpiler rewrites this to the loader's `solidNotifications` context property (D-Bus worker
// thread on Linux); this web mirror only keeps shared sources typechecking — the browser
// preview reports the capability as absent.
const noSignal = { connect() { /* native-only */ }, disconnect() { /* native-only */ } };
export const notifications = {
  available: false,
  supportsActions: false,
  supportsReply: false,
  send: (_spec: Record<string, unknown>) => 0,
  close: (_id: number) => { /* native-only */ },
  actionInvoked: noSignal,
  replied: noSignal,
  closed: noSignal,
};

export function qmlComponent(path: string) {
  return function QmlImportedComponent(props: PrimitiveProps) {
    return (
      <div
        id={props.id}
        class={classes(props, "qml-import")}
        data-qml-type="ImportedQml"
        data-qml-source={path}
      >
        {props.children}
      </div>
    );
  };
}
