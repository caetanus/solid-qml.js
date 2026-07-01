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
