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

/** File IO result of the DocumentEditor ref methods. */
type DocumentIoResult = { ok: boolean; error: string };

type DocumentEditorProps = {
  class?: string;
  /** `let doc: DocumentEditorHandle | undefined; <DocumentEditor ref={doc} />` */
  ref?: DocumentEditorHandle;
  /** false hides the built-in toolbar (the app draws its own). Default true. */
  toolbar?: boolean;
  onModifiedChanged?: (modified: boolean) => void;
  /** Debounced; no payload — read doc.plainText() / doc.toHtml() when needed. */
  onContentChanged?: () => void;
  /** The style table changed (defineStyle, a load, newDocument, an undo of a definition). */
  onStylesChanged?: () => void;
  /** Page width in px (e.g. 794 ≈ A4 at 96dpi). 0/unset = the plain editor's default. The editor
   *  is continuous (no on-screen page breaks) either way. */
  pageWidth?: number;
  /** [top, right, bottom, left] px. null/unset = the plain editor's default (12px uniform). */
  pageMargins?: [number, number, number, number] | null;
  /** Clamped 0.5..3.0. A REAL relayout (font/point sizes, image sizes rebuilt, not a scale
   *  transform): saveOdf's output does not depend on it. The app owns the value. */
  zoom?: number;
  /** Ctrl+wheel / Ctrl+= / Ctrl+- / Ctrl+0: +0.1 / -0.1 / 0 (reset). The app applies it to `zoom`. */
  onZoomRequested?: (delta: number) => void;
};

/** A named style's properties (docs/document-editor.md "Named styles"). Lengths are px (or a
 *  string with a unit: "1cm", "12pt"); fontSize is in points. */
type DocumentStyleProps = {
  /** New styles only (default "paragraph"). */
  family?: "paragraph" | "character";
  parent?: string | null;
  /** Paragraph styles: the style Enter gives the next paragraph. */
  nextStyle?: string | null;
  fontFamily?: string | null;
  fontSize?: number | string | null;
  fontWeight?: number | "bold" | "normal" | null;
  italic?: boolean | null;
  underline?: boolean | null;
  strike?: boolean | null;
  /** "#rrggbb" */
  color?: string | null;
  letterSpacing?: number | string | null;
  marginTop?: number | string | null;
  marginBottom?: number | string | null;
  marginLeft?: number | string | null;
  marginRight?: number | string | null;
  textIndent?: number | string | null;
  /** A multiple of the line (1.5, "150%") or an exact height ("18pt", "24px"). */
  lineHeight?: number | string | null;
  alignment?: "left" | "center" | "right" | "justify" | null;
  keepWithNext?: boolean | null;
  /** 1..10 makes it a heading style (0 = not a heading). */
  outlineLevel?: number | null;
};

/** directFormattingAt(): what overrides the styles (null = a property the style sets and the
 *  text removes). */
type DocumentDirectFormatting = {
  paragraphStyle: string;
  charStyle: string;
  paragraph: Partial<Record<keyof DocumentStyleProps, any>>;
  character: Partial<Record<keyof DocumentStyleProps, any>>;
};

declare global {
  const BusyIndicator: NativeWidget;
  const Chart: NativeWidget;
  const CodeEditor: NativeWidget;
  const ContextMenu: NativeWidget;
  const DelayButton: NativeWidget;
  const DocumentEditor: Component<DocumentEditorProps>;
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

  /** What `ref={doc}` on <DocumentEditor> gives the app (see docs/document-editor.md). */
  interface DocumentEditorHandle {
    readonly modified: boolean;
    readonly bold: boolean;
    readonly italic: boolean;
    readonly underline: boolean;
    readonly strike: boolean;
    readonly bulletList: boolean;
    /** 0 = body text, 1..6 = heading level. */
    readonly heading: number;
    readonly alignment: "left" | "center" | "right" | "justify";
    readonly inTable: boolean;
    /** The caret's paragraph style ("Default Paragraph Style", "Text Body", "Heading 1", …). */
    readonly currentParagraphStyle: string;
    /** The character style at the caret ("" = none). */
    readonly currentCharStyle: string;
    readonly canUndo: boolean;
    readonly canRedo: boolean;
    /** A plain path or a file:// URL. */
    loadOdf(path: string): DocumentIoResult;
    saveOdf(path: string): DocumentIoResult;
    /** Same .odt as saveOdf, for autosave / crash recovery: `modified` and undo stay untouched. */
    saveOdfCopy(path: string): DocumentIoResult;
    newDocument(): void;
    toHtml(): string;
    plainText(): string;
    insertText(text: string): void;
    focusEditor(): void;
    undo(): void;
    redo(): void;
    toggleBold(): void;
    toggleItalic(): void;
    toggleUnderline(): void;
    toggleStrike(): void;
    toggleBulletList(): void;
    /** 0 = "Text Body", 1..6 = "Heading n" (applyParagraphStyle). */
    setHeading(level: number): void;
    setAlignment(align: "left" | "center" | "right" | "justify"): void;
    insertTable(rows: number, cols: number): void;
    addTableRow(): void;
    addTableColumn(): void;
    removeTableRow(): void;
    removeTableColumn(): void;
    insertImage(path: string): DocumentIoResult;
    /** Create / update a named style; paragraphs and runs using it follow (direct formatting kept). */
    defineStyle(name: string, props: DocumentStyleProps): DocumentIoResult;
    styleNames(family: "paragraph" | "character"): string[];
    /** Own properties (+ family, parent, nextStyle); `resolved` = what the whole chain gives. */
    styleProperties(name: string, resolved?: boolean): DocumentStyleProps;
    /** On the paragraphs the selection touches. false for an unknown / character style. */
    applyParagraphStyle(name: string): boolean;
    /** On the selection (the word at the caret without one); "" removes the character style. */
    applyCharStyle(name: string): boolean;
    /** The selection (or the caret's paragraph) back to exactly what its styles give. */
    clearDirectFormatting(): void;
    /** Overrides on top of the styles at `pos` (default: the caret). */
    directFormattingAt(pos?: number): DocumentDirectFormatting;
  }

  // Runtime CSS-theme override handle, injected by the loader (see examples/dashboard).
  const cssTheme: { loadLayeredString(css: string): void };
}

// Module declarations for `qml-solid` and `*.qml` live in ./ambient-modules.d.ts (a script-mode
// ambient file — those declarations must not sit inside this module-mode file to be picked up).
