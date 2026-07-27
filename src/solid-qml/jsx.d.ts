import type { JSX, Component } from "solid-js";

declare module "solid-js" {
  namespace JSX {
    interface IntrinsicElements {
      Window: WindowProps;
      div: QmlDivProps;
      span: QmlTextProps;
      text: QmlTextProps;
      hr: QmlHrProps;
      button: QmlButtonProps;
      combo: QmlControlProps;
      radio: QmlControlProps;
      check: QmlControlProps;
      // HTML-derived native tags the transpiler lowers to widgets (lowercase → intrinsic).
      details: QmlControlProps;
      dialog: QmlControlProps;
      fieldset: QmlControlProps;
      legend: QmlControlProps;
      progress: QmlControlProps;
    }
  }
}

type BaseQmlProps = {
  id?: string;
  class?: string;
  className?: string;
  children?: JSX.Element;
};

type WindowProps = BaseQmlProps & {
  title?: string;
  width?: number;
  height?: number;
  visible?: boolean;
};

type QmlDivProps = BaseQmlProps &
  Omit<JSX.HTMLAttributes<HTMLDivElement>, "id" | "class" | "className" | "onDrop"> & {
    // Drag-and-drop payload carried on a draggable box (see examples/dashboard reorder). Unlike
    // the DOM, the drop handler receives the SOURCE's dragData (not a DragEvent) — see qml.ts Props.
    dragData?: unknown;
    onDrop?: (dragData: any) => void;
  };

type QmlTextProps = BaseQmlProps &
  Omit<JSX.HTMLAttributes<HTMLElement>, "id" | "class" | "className">;

type QmlHrProps = Omit<BaseQmlProps, "children"> &
  Omit<JSX.HTMLAttributes<HTMLHRElement>, "id" | "class" | "className" | "children">;

type QmlButtonProps = BaseQmlProps &
  Omit<JSX.ButtonHTMLAttributes<HTMLButtonElement>, "id" | "class" | "className"> & {
    onClick?: JSX.EventHandlerUnion<HTMLButtonElement, MouseEvent>;
    onClicked?: JSX.EventHandlerUnion<HTMLButtonElement, MouseEvent>;
  };

type QmlControlProps = BaseQmlProps & Record<string, unknown>;

// ─── Native widget tags ──────────────────────────────────────────────────────────────────────
// Capitalized tags the transpiler recognizes and lowers to `solidqml.Widgets` components. They
// are used WITHOUT an import (QML-only surface; the browser build guards these subtrees out), so
// TSX needs the names to exist as ambient values for `<ToolBar>` etc. to typecheck. Props are
// permissive: the common ones are typed (so handler params infer) and any extra prop is allowed.
type NativeWidgetProps = {
  id?: string;
  class?: string;
  className?: string;
  children?: JSX.Element;
  // Handlers are declared so their params infer; widget onChange arity varies (Dial → (v),
  // RangeSlider → (lo, hi)) so it is variadic. Every other prop is `any` via the index signature.
  onChange?: (...args: any[]) => void;
  onClick?: (...args: any[]) => void;
  onSelect?: (...args: any[]) => void;
  [key: string]: any;
};
type NativeWidget = Component<NativeWidgetProps>;

declare global {
  const BusyIndicator: NativeWidget;
  const Chart: NativeWidget;
  const CodeEditor: NativeWidget;
  const ContextMenu: NativeWidget;
  const DelayButton: NativeWidget;
  const Dial: NativeWidget;
  const Drawer: NativeWidget;
  const ListView: NativeWidget;
  const MediaPlayer: NativeWidget;
  const Menu: NativeWidget;
  const MenuBar: NativeWidget;
  const MenuItem: NativeWidget;
  const MenuSeparator: NativeWidget;
  const PageIndicator: NativeWidget;
  const RangeSlider: NativeWidget;
  const RichText: NativeWidget;
  const RoundButton: NativeWidget;
  const Scene3D: NativeWidget;
  const Shortcut: NativeWidget;
  const SplitView: NativeWidget;
  const StackView: NativeWidget;
  const Surface: NativeWidget;
  const SwipeView: NativeWidget;
  const TabBar: NativeWidget;
  const TabButton: NativeWidget;
  const TableView: NativeWidget;
  const ToolBar: NativeWidget;
  const ToolButton: NativeWidget;
  const ToolSeparator: NativeWidget;
  const Tray: NativeWidget;
  const TreeView: NativeWidget;
  const Tumbler: NativeWidget;
  const WebView: NativeWidget;

  // Runtime CSS-theme override handle, injected by the loader (see examples/dashboard).
  const cssTheme: { loadLayeredString(css: string): void };
}

// Module declarations for `qml-solid` and `*.qml` live in ./ambient-modules.d.ts (a script-mode
// ambient file — those declarations must not sit inside this module-mode file to be picked up).
