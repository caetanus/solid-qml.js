import type { JSX, Component, Accessor } from "solid-js";

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

  /** <For> row identity (C23) is the element's REFERENCE, like Solid web: a new object is a new row
   *  (the old row's local state resets, its onCleanup runs) even when it carries the same id.
   *  NATIVE-ONLY `key`: `key="id"` (or `key={(r) => r.id}`) reuses the row of a new object whose
   *  field matches and updates it in place. Solid web's <For> has no `key` prop and ignores it —
   *  rows recreate by reference there: the one divergence an app opts into. */
  export function For<T extends readonly any[], U extends JSX.Element>(props: {
    each: T | undefined | null | false;
    fallback?: JSX.Element;
    key?: (keyof T[number] & string) | ((item: T[number]) => unknown);
    children: (item: T[number], index: Accessor<number>) => U;
  }): JSX.Element;
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
  classList?: Record<string, boolean | undefined>;
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
   *  displays paginated sheets by default. */
  pageWidth?: number;
  /** [top, right, bottom, left] px. null/unset = the plain editor's default (12px uniform). */
  pageMargins?: [number, number, number, number] | null;
  /** Clamped 0.5..3.0. A REAL relayout (font/point sizes, image sizes rebuilt, not a scale
   *  transform): saveOdf's output does not depend on it. The app owns the value. */
  zoom?: number;
  /** Ctrl+wheel / Ctrl+= / Ctrl+- / Ctrl+0: +0.1 / -0.1 / 0 (reset). The app applies it to `zoom`. */
  onZoomRequested?: (delta: number) => void;
  /** The outline changed (doc.headings()) — debounced, and only when the list actually differs. */
  onHeadingsChanged?: () => void;
  /** Every block was replaced (loadOdf, newDocument): the app's first full pass (doc.blockIds()).
   *  No onBlockRemoved/onBlockChanged is sent for that. */
  onBlocksLoaded?: () => void;
  /** A block's TEXT changed, or a block appeared (Enter, paste, undo). Coalesced per block,
   *  delivered on the next event-loop turn; never for formatting or marks. */
  onBlockChanged?: (blockId: string) => void;
  /** The block left the document (merged, deleted, an undone split): drop its state. */
  onBlockRemoved?: (blockId: string) => void;
  /** A click on a mark; x, y in the editor's coordinates, windowX/windowY in the window's (what
   *  a `<Menu ref>`'s open(x, y) takes). */
  onMarkClicked?: (mark: DocumentMark, x: number, y: number, windowX: number, windowY: number) => void;
  /** A right-click on a mark (show suggestions; open a menu right here — a Wayland popup needs
   *  the click's input serial). A right-click elsewhere is left alone. */
  onMarkContextMenu?: (mark: DocumentMark, x: number, y: number, windowX: number, windowY: number) => void;
  /** The pointer entered a mark, or left it (null). */
  onMarkHovered?: (mark: DocumentMark | null) => void;
};

/** A mark (docs/document-editor.md "Blocks and marks"): [start, end) are UTF-16 offsets in the
 *  block's text (doc.blockText(blockId)). */
type DocumentMark = { kind: string; id: string; blockId: string; start: number; end: number };

/** How a kind of mark is drawn. Left out: kept; null: removed. Defaults: "spelling" a red wave,
 *  "grammar" a blue wave, "highlight" a yellow background, any other kind a grey underline. */
type DocumentMarkStyle = {
  underline?: "wave" | "single" | "none" | null;
  /** The underline's colour ("#rrggbb" or a color name). */
  color?: string | null;
  /** Drawn under the text. */
  background?: string | null;
};

/** One entry of doc.headings() (docs/document-editor.md "Outline"). */
type DocumentHeading = { position: number; level: number; text: string; blockId: string };

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
    /** View geometry in layout pixels at zoom 1; configure through the ref or QML. */
    pageHeight: number;
    pageGap: number;
    pageView: boolean;
    deskColor: string;
    readonly pageCount: number;
    /** One-based page containing the caret. */
    readonly currentPage: number;
    pageAt(position: number): number;
    scrollToPage(page: number): void;
    /** QML signal; use doc.pagesChanged.connect(callback) from the ref. */
    readonly pagesChanged: { connect(callback: () => void): void; disconnect(callback: () => void): void };
    readonly modified: boolean;
    readonly bold: boolean;
    readonly italic: boolean;
    readonly underline: boolean;
    readonly strike: boolean;
    readonly bulletList: boolean;
    /** Mutually exclusive with bulletList. */
    readonly numberedList: boolean;
    /** 0 = body text, 1..6 = heading level. */
    readonly heading: number;
    /** "" when unset (Default Paragraph Style carries no explicit font by design). */
    readonly fontFamily: string;
    /** pt, canonical (zoom-1) units; 0 when unset. */
    readonly fontSize: number;
    readonly alignment: "left" | "center" | "right" | "justify";
    readonly inTable: boolean;
    /** The caret's paragraph style ("Default Paragraph Style", "Text Body", "Heading 1", …). */
    readonly currentParagraphStyle: string;
    /** The character style at the caret ("" = none). */
    readonly currentCharStyle: string;
    readonly canUndo: boolean;
    readonly canRedo: boolean;
    /** The SYSTEM clipboard (not this document) currently carries pasteable content. */
    readonly canPaste: boolean;
    /** Whole document, kept incrementally (not recounted per keystroke). A "word" is a Unicode
     *  word-boundary segment starting with a letter or digit; "chars" excludes the paragraph
     *  separator between blocks. */
    readonly wordCount: number;
    readonly charCount: number;
    /** The current selection (0 with none); same word/char rules as above. */
    readonly selectionWordCount: number;
    readonly selectionCharCount: number;
    /** "" with no selection; paragraphs separated by "\n", like plainText(). */
    readonly selectedText: string;
    /** Index into headings() of the heading at/before the caret, -1 if none. */
    readonly currentHeadingIndex: number;
    /** [{position, level, text, blockId}] in document order (level: the outline/heading level). Kept
     *  incrementally by the engine — cheap even on a very large document. */
    headings(): DocumentHeading[];
    /** Scrolls so `pos` sits near the top of the viewport. */
    scrollToPosition(pos: number): void;
    /** A plain path or a file:// URL. */
    loadOdf(path: string): DocumentIoResult;
    saveOdf(path: string): DocumentIoResult;
    /** Same .odt as saveOdf, for autosave / crash recovery: `modified` and undo stay untouched. */
    saveOdfCopy(path: string): DocumentIoResult;
    /** Snapshot on the UI thread, atomic write on a worker; busy requests coalesce, latest wins. */
    saveOdfCopyAsync(path: string): void;
    readonly saveCopyFinished: {
      connect(callback: (ok: boolean, error: string) => void): void;
      disconnect(callback: (ok: boolean, error: string) => void): void;
    };
    /** Empty, unmodified document in Default Paragraph Style; drops undo history, images and
     *  style definitions (back to the built-in styles) — same as `newDocument()`. With `options`,
     *  the styles are defined/updated first (same validation as `defineStyle`; any error leaves
     *  the PREVIOUS document untouched and returns `{ok: false, error}`) and the empty first
     *  paragraph starts in `paragraphStyle` (default: Default Paragraph Style). Still not an undo
     *  step; `modified` is still cleared; the definitions are written to styles.xml on save like
     *  any other. */
    newDocument(options?: { styles?: Record<string, DocumentStyleProps>; paragraphStyle?: string }): DocumentIoResult;
    toHtml(): string;
    plainText(): string;
    insertText(text: string): void;
    focusEditor(): void;
    undo(): void;
    redo(): void;
    /** HTML (formatting kept, rescaled to canonical zoom-1 sizes) and plain text; no-op without a
     *  selection. Ctrl+C/X/V are routed to these (not the editor's own built-in handling) so the
     *  zoom rescaling below applies either way — these are also there for an app's own toolbar. */
    cut(): void;
    copy(): void;
    /** Prefers HTML when the clipboard has it, else plain text; no-op when !canPaste. HTML goes
     *  through insertHtml() (rescaled to the zoom in effect); plain text takes the caret's own
     *  already-zoomed character format. */
    paste(): void;
    /** Inserts HTML at the caret (replacing a selection), rescaled to the zoom in effect — the
     *  same choke point paste() uses for its HTML branch. */
    insertHtml(html: string): void;
    toggleBold(): void;
    toggleItalic(): void;
    toggleUnderline(): void;
    toggleStrike(): void;
    toggleBulletList(): void;
    /** Mirrors toggleBulletList() exactly — see the handle's numberedList doc. */
    toggleNumberedList(): void;
    /** In a list: one level deeper/shallower; otherwise the paragraph's own left margin, one
     *  step. outdent() past the first list level detaches it (plain paragraph). */
    indent(): void;
    outdent(): void;
    /** Splits the paragraph at the caret and marks the new one to start on a fresh page
     *  (written to / read from ODF as fo:break-before="page"). Shown as a dashed marker in the continuous view. */
    insertPageBreak(): void;
    /** Label on page-break overlay markers; default "Quebra de página". */
    pageBreakLabel: string;
    /** 0 = "Text Body", 1..6 = "Heading n" (applyParagraphStyle). */
    setHeading(level: number): void;
    /** On the selection, or only on what is typed next with nothing selected.
     *  Reports the pending font immediately; moving the caret cancels it. */
    setFontFamily(name: string): void;
    /** pt, canonical (zoom-1) units. Same mechanics as setFontFamily. */
    setFontSize(pt: number): void;
    /** Drops direct CHARACTER formatting (bold, font, size, …) in the selection; keeps character
     *  styles and leaves paragraph properties untouched (unlike clearDirectFormatting()). */
    clearFormatting(): void;
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
    /** Every block's stable id, in document order (kept through typing, other blocks' edits and
     *  undo/redo; a split gives the part after the caret a new id; saved as xml:id). */
    blockIds(): string[];
    /** The id of the block holding document position `pos` ("" outside the document). */
    blockIdAt(pos: number): string;
    /** The block's plain text (UTF-16 — mark offsets count in it; a soft line break reads "\n").
     *  "" for an unknown id. */
    blockText(blockId: string): string;
    /** Marks [start, end) of the block's text; returns the mark's id (`id`, or a generated one),
     *  "" for an unknown block or a range outside its text. An overlay: never saved, copied,
     *  undone nor counted as a modification. Edits move it with the text; an edit inside it (or
     *  deleting any of it) removes it. */
    addMark(kind: string, blockId: string, start: number, end: number, id?: string): string;
    /** Every mark of `kind`, or only those in `blockId`. Other kinds/blocks are untouched. */
    clearMarks(kind: string, blockId?: string): void;
    /** How `kind` is drawn (see DocumentMarkStyle). */
    markStyle(kind: string, style: DocumentMarkStyle): DocumentIoResult;
    /** The mark over the character at `pos`, or null. */
    markAt(pos: number): DocumentMark | null;
    /** Replaces [start, end) of the block's text (e.g. a suggestion) — one undo step, the block
     *  keeps its id; "\n" starts a paragraph. false for an unknown id / a range outside the text. */
    replaceRange(blockId: string, start: number, end: number, text: string): boolean;
  }

  // Runtime CSS-theme override handle, injected by the loader (see examples/dashboard).
  const cssTheme: { loadLayeredString(css: string): void };
}

// Module declarations for `qml-solid` and `*.qml` live in ./ambient-modules.d.ts (a script-mode
// ambient file — those declarations must not sit inside this module-mode file to be picked up).
