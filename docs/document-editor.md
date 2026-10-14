# Document editor

`<DocumentEditor>` is the editing surface of a word processor: the **app** drives the document —
opens and saves it, draws its own toolbar from the format state, and reacts to edits. Body text
and headings are **named styles** (LibreOffice's: "Text Body", "Heading 1", …), not direct
formatting. It shares its engine with `<RichText>` (module `solidqml.Widgets.RichText`: a Qt
`TextEdit` plus the loader's `RichTextHandler`, with our own OpenDocument reader and writer).
`<RichText>` stays as it is for mail and forms (`html` in, `onChange(html, text)` out).

It is native-only (no browser equivalent); the module is imported only when the tag is used.

The caret follows keyboard movement, typing, paste, undo/redo, and programmatic selection with
one line of context where space permits. PageUp/PageDown move by a viewport; Shift extends the
selection, and Ctrl+Home/End reach the document boundaries. `setCursorPosition(pos)` moves the
caret and reveals it. `scrollToPosition(pos)` only moves the viewport. Wheel and scrollbar
movement do not snap back to the caret. This works in page and continuous views at every zoom.

The vertical scrollbar shares the CSS engine's Controls-free desktop implementation, including
its hover/pressed appearance, track paging, and thumb dragging. Its range follows the full document
extent even when pages and text rendering are culled. Both editor tags accept dynamic `class`
strings and `classList`, for example `class="sheet" classList={{ paged: pageView() }}`.

## Usage

```tsx
import { createSignal } from "solid-js";

export function Writer() {
  let doc!: DocumentEditorHandle;
  const [dirty, setDirty] = createSignal(false);
  const [status, setStatus] = createSignal("");

  const save = () => {
    const r = doc.saveOdf("/home/me/notes.odt");   // a plain path or a file:// URL
    setStatus(r.ok ? "Saved" : "Save failed: " + r.error);
  };

  return (
    <div class="writer">
      <div class="bar">
        <button classList={{ on: doc.bold }} onClick={() => doc.toggleBold()}>B</button>
        <button classList={{ on: doc.currentParagraphStyle === "Heading 1" }}
          onClick={() => doc.applyParagraphStyle("Heading 1")}>H1</button>
        <button onClick={save}>Save</button>
      </div>
      <DocumentEditor ref={doc} class="editor" toolbar={false}
        onModifiedChanged={(m) => setDirty(m)}
        onContentChanged={() => console.log(doc.plainText().length)} />
      <text>{(dirty() ? "● " : "") + status()}</text>
    </div>
  );
}
```

A complete example (own toolbar, open/save by path, modified dot, word count) is
`examples/document-editor.tsx`, with the window entry `examples/document-editor.qml.tsx`.

## Attributes

| Attribute | Value | |
|---|---|---|
| `class` | string literal | CSS classes (size it like any box). |
| `ref` | variable | `let doc!: DocumentEditorHandle` — the handle below. |
| `toolbar` | boolean (binding) | `false` hides the built-in toolbar. Default `true`. |
| `onModifiedChanged` | `(modified: boolean) => …` | The unsaved-changes flag flipped. |
| `onContentChanged` | `() => …` | Any edit, formatting included, **debounced** (300 ms of quiet). No payload: read `doc.plainText()` / `doc.toHtml()` when needed. Opening a file, `newDocument()` and `defineStyle()` count as content changes. |
| `onStylesChanged` | `() => …` | The style table changed: `defineStyle`, a load, `newDocument`, an undo/redo of a definition. Refresh a style picker from `doc.styleNames(…)` here. |
| `pageWidth` | number (px) | Sheet width at zoom 1 (e.g. `794` ≈ A4 at 96dpi). `0`/unset uses 794px sheets; in continuous view it keeps the plain editor's default. |
| `pageMargins` | `[top, right, bottom, left]` (px), or `null`/unset | Text margins repeated on every sheet; the caret and selection respect them. `null`/unset keeps the plain editor's default (12px uniform). |
| `zoom` | number, clamped `0.5`–`3.0` | A **real relayout**: font/point sizes, margins and inline images are rebuilt at the new size (not a scale transform — text stays sharp). Does not change what `saveOdf` writes. The app owns the value; see `onZoomRequested`. |
| `onZoomRequested` | `(delta: number) => …` | Ctrl+wheel (±0.1 per notch), Ctrl+= / Ctrl+- (±0.1), Ctrl+0 (`0`, meaning "reset"). The editor only asks; set `zoom` in response (e.g. `setZoom(z => delta === 0 ? 1 : clamp(z + delta))`). |
| `onHeadingsChanged` | `() => …` | The outline changed, **debounced** (300 ms) and only when `doc.headings()` actually differs from before. Refresh a table of contents from `doc.headings()` here. |
| `onBlocksLoaded` | `() => …` | Every block was replaced: after `loadOdf`, `newDocument` (and when the editor first gets its document). The app's first full pass — walk `doc.blockIds()`. No `onBlockRemoved`/`onBlockChanged` is sent for the blocks a load replaced. See "Blocks and marks". |
| `onBlockChanged` | `(blockId: string) => …` | A block's **text** changed, or a block appeared (Enter, a paste, an undo that brings one back). Coalesced per block and delivered on the next event-loop turn (three keystrokes in one paragraph → one call); never for a formatting-only change or for marks. |
| `onBlockRemoved` | `(blockId: string) => …` | The block left the document (merged into its neighbour, deleted, an undone split): drop the app's state for it. Same delivery as `onBlockChanged`, before it. |
| `onMarkClicked` | `(mark, x, y, windowX, windowY) => …` | A click on a mark. `mark` is `{kind, id, blockId, start, end}`; `x, y` in the editor's coordinates, `windowX, windowY` the same point in the window's (what a `<Menu ref>`'s `open(x, y)` takes). The caret moves as usual. |
| `onMarkContextMenu` | `(mark, x, y, windowX, windowY) => …` | A right-click on a mark — show suggestions here, synchronously (a Wayland popup needs the click's input serial). A right-click elsewhere is left alone. |
| `onMarkHovered` | `(mark \| null) => …` | The pointer entered a mark, or left it (`null`). |

The editor starts **empty and unmodified** (no demo text). Anything else is a transpile error.

## The handle (`ref`)

State — reactive: reading `doc.bold` in a binding (a `classList`, a text) follows the caret.

| Property | Type | |
|---|---|---|
| `modified` | boolean | Unsaved edits. Cleared by `loadOdf`, `saveOdf`, `newDocument` (not by `saveOdfCopy`). |
| `bold`, `italic`, `underline`, `strike` | boolean | Character format at the caret / selection. |
| `heading` | number | `0` not a heading, `1`–`6` heading (outline) level. |
| `fontFamily` | string | The EFFECTIVE value at the caret / selection: direct formatting, else the resolved style chain (including a Default Paragraph Style the app itself customized via `newDocument(options)`); `""` when nothing anywhere defines it (the editor's own font stands in) or the selection's runs disagree (Word's "blank box" for a mixed selection). |
| `fontSize` | number | Same resolution as `fontFamily`, in canonical (zoom-1) units — like a word processor's font-size box, the same number at any zoom; `0` under the same unset/mixed rules. |
| `currentParagraphStyle` | string | The caret's paragraph style (`"Text Body"`, `"Heading 1"`, …). |
| `currentCharStyle` | string | The character style at the caret, `""` for none. |
| `alignment` | `"left" \| "center" \| "right" \| "justify"` | Paragraph alignment. |
| `bulletList`, `numberedList`, `inTable` | boolean | The caret is in a bullet / numbered list (mutually exclusive) / a table. |
| `canUndo`, `canRedo` | boolean | |
| `canPaste` | boolean | The SYSTEM clipboard (not this document) currently carries pasteable content — tracks another app too. |
| `wordCount`, `charCount` | number | Whole document, kept incrementally (see "Counts" below) and notified (`contentsChanged`) after `loadOdf`/`newDocument`/attaching a document, not just a keystroke. A word is a Unicode word-boundary segment starting with a letter or digit; chars excludes the paragraph separator between blocks. |
| `selectionWordCount`, `selectionCharCount` | number | The current selection, by the same rules; `0` with none. |
| `cursorPosition` | number | Caret position in UTF-16 document characters, using the same coordinates as `setCursorPosition` and `blockIdAt`. |
| `selectionStart`, `selectionEnd` | number | Ordered endpoints of the selection (text in `[selectionStart, selectionEnd)`); both equal the caret when collapsed. Each property is reactive and emits its own `…Changed()` signal only when its value changes, including cursor movement, typing, and document replacement. Connect through e.g. `doc.selectionEndChanged.connect(callback)` on the ref. |
| `selectedText` | string | `""` with no selection; paragraphs separated by `"\n"`, like `plainText()`. |
| `currentHeadingIndex` | number | Index into `headings()` of the heading at/before the caret, `-1` if none. |

Methods:

| Method | Returns | |
|---|---|---|
| `headings()` | `{position, level, text, blockId}[]` | The outline, in document order; `blockId` is the heading block's stable id (key an outline `<For key="blockId">` on it). Kept incrementally by the engine (see "Outline" below) — cheap to call even on a very large document. |
| `scrollToPosition(pos)` | | Scrolls so `pos` sits near the top of the viewport (e.g. `doc.scrollToPosition(doc.headings()[i].position)`). |
| `loadOdf(path)` | `{ ok, error }` | Opens an `.odt`, with its named styles. On failure (`file not found: …`, `not an OpenDocument file: …`) the current document is kept. The load is not an undo step. |
| `saveOdf(path)` | `{ ok, error }` | Writes an `.odt` (our writer: named styles, heading levels, images embedded; atomic — a failed write leaves the old file). Clears `modified` on success. |
| `saveOdfCopy(path)` | `{ ok, error }` | The same `.odt`, for autosave / crash recovery: the document is left as it was — `modified` stays, the undo history is untouched. |
| `saveOdfCopyAsync(path)` | void | Snapshots the document on the UI thread, then writes atomically on a worker. Emits `saveCopyFinished(ok, error)` on the UI thread; never changes modified or undo. While busy, only the latest requested path is retained; its snapshot is taken when the previous write finishes. Each actual write emits one completion. Connect through `doc.saveCopyFinished.connect(callback)` on the ref. |
| `newDocument(options?)` | `{ ok, error }` | Empty, unmodified document in Default Paragraph Style; drops undo history, images and style definitions (back to the built-in styles). With `options`, styles are defined first (see "Starting a document with styles" below) and `{ ok, error }` reports it, like `loadOdf`. |
| `toHtml()` / `plainText()` | string | The content; `plainText()` separates paragraphs with `\n`. |
| `insertText(text)` | | At the caret (replacing a selection); `\n` starts a paragraph, as Enter does (next style included). |
| `toggleBold()`, `toggleItalic()`, `toggleUnderline()`, `toggleStrike()` | | On the selection; with nothing selected, on what is typed next (as the toolbar does). |
| `setHeading(level)` | | `applyParagraphStyle("Heading " + level)`; `0` → `"Text Body"`. |
| `setFontFamily(name)`, `setFontSize(pt)` | | On the selection; with nothing selected, only on what is typed next; existing characters never change. `fontFamily`/`fontSize` report the pending value immediately; moving the caret cancels it. `pt` is canonical (zoom-1) units, applied at the zoom in effect. |
| `clearFormatting()` | | Drops direct CHARACTER formatting (bold, font, size, …) in the selection; keeps character styles. Unlike `clearDirectFormatting()`, paragraph properties (alignment, margins, …) are untouched. |
| `setAlignment(align)` | | On every paragraph the selection touches. |
| `toggleBulletList()`, `toggleNumberedList()` | | Each mirrors the other exactly: in ANY list, detach it (plain paragraph); otherwise start one of its own kind. Toggling the other kind while already in a list turns the list off, not a conversion. |
| `indent()`, `outdent()` | | In a list: one level deeper/shallower (`outdent()` past the first level detaches it). Otherwise: the paragraph's own left margin, one ~24px step, at the zoom in effect. |
| `insertPageBreak()` | | Splits the paragraph at the caret (`insertParagraph()`'s mechanics) and marks the new one to start on a fresh page. Starts a new sheet in page view. In continuous view, shown above the new block as a dashed overlay labelled "Quebra de página". The `pageBreakLabel` QML/ref property overrides the label. Markers follow scrolling and zoom, never enter saved content or undo, and the break round-trips through `saveOdf`/`loadOdf` as `fo:break-before="page"`. |
| `insertTable(rows, cols)`, `addTableRow()`, `addTableColumn()`, `removeTableRow()`, `removeTableColumn()` | | Row/column edits act on the cell at the caret. |
| `insertImage(path)` | `{ ok, error }` | Inline image at the caret, capped to the column width. |
| `undo()`, `redo()`, `focusEditor()` | | |
| `cut()`, `copy()`, `paste()` | | HTML (formatting kept, rescaled to canonical zoom-1 sizes — see "Clipboard" below) and plain text on the SYSTEM clipboard; `cut()`/`copy()` are no-ops without a selection, `paste()` when `!canPaste`. Ctrl+C/X/V are routed to these (not the editor's own built-in handling) — these are also there for an app's own toolbar/menu. |
| `insertHtml(html)` | | At the caret (replacing a selection), rescaled to the zoom in effect — the same choke point `paste()` uses for its HTML branch. |
| `blockIds()` | `string[]` | Every block's id, in document order (see "Blocks and marks"). |
| `blockIdsInRange(from, to)` | `string[]` | Block ids intersecting inclusive `[from, to]`, in document order, including table-cell paragraphs. UTF-16 document positions, like `blockIdAt`; paragraph separators belong to the preceding block, and a collapsed range returns its containing block (including an empty paragraph). Clips to the document; reversed or wholly outside ranges return `[]`. Uses indexed lookup plus traversal of only the intersecting blocks, so small ranges stay cheap in large documents. |
| `blockIdAt(pos)` | string | The id of the block holding document position `pos`; `""` outside the document. |
| `blockText(blockId)` | string | The block's plain text, UTF-16 — the space mark offsets count in (a soft line break reads as `"\n"`, same length). `""` for an unknown id. |
| `addMark(kind, blockId, start, end, id?)` | string | Marks `[start, end)` of the block's text; returns the mark's id (`id`, or a generated one), `""` for an unknown block or a range outside its text. Ids are the app's: not checked for uniqueness. |
| `removeMark(id)` | boolean | Removes one mark with this id, regardless of kind; `false` when unknown. Leaves all other marks untouched and repaints the overlay. Never changes document content, undo/redo or `modified`. If caller-supplied ids are duplicated, removes only one match; use unique ids to identify a particular mark. |
| `clearMarks(kind, blockId?)` | | Every mark of `kind`, or only those in `blockId`. Other kinds and blocks are untouched. |
| `markStyle(kind, style)` | `{ ok, error }` | How `kind` is drawn: `{ underline: "wave" \| "single" \| "none", color, background }` — left out: kept; `null`: removed. An error (`underline: expected …`, `color: not a color …`, an unknown property) changes nothing. |
| `markAt(pos)` | `{kind, id, blockId, start, end}` or `null` | The mark over the character at `pos`. |
| `replaceRange(blockId, start, end, text)` | boolean | Replaces `[start, end)` of the block's text (apply a suggestion): one undo step, the block keeps its id, the replaced text's character format carries over; `"\n"` starts a paragraph (as `insertText`). `false` for an unknown id or a range outside the text. |

Formatting calls give the keyboard back to the editor, so an app toolbar button does not leave
the caret stranded. Ctrl+B / Ctrl+I / Ctrl+U work with or without the built-in toolbar.

## Named styles

Every paragraph is in one **paragraph style**; a run of text may be in a **character style**. What
you see is the style chain plus **direct formatting** (bold from Ctrl+B, an alignment button, …):
direct formatting is the exception on top, and one call clears it. The names are LibreOffice's,
so files go both ways and LibreOffice lists them as real styles.

Built in: paragraph styles `Default Paragraph Style` (the root every paragraph style inherits),
`Text Body`, `Heading` (the headings' parent), `Heading 1`…`Heading 6`, `Title`, `Quotations`;
character styles `Emphasis` and `Strong Emphasis`. A new document is in Default Paragraph Style;
Enter at the end of a heading starts `Text Body` (the heading's next style).

```tsx
doc.defineStyle("Text Body", { fontSize: 12, lineHeight: 1.5, alignment: "justify", marginBottom: "0.25cm" });
doc.defineStyle("Note", { parent: "Text Body", italic: true, nextStyle: "Text Body" });
doc.defineStyle("Code", { family: "character", fontFamily: "Monospace", color: "#7a3e00" });
doc.applyParagraphStyle("Heading 2");
doc.applyCharStyle("Code");
```

| Method | Returns | |
|---|---|---|
| `defineStyle(name, props)` | `{ ok, error }` | Creates (`family` `"paragraph"` — default — or `"character"`) or updates a style. Properties left out are kept, `null` removes one. Every paragraph / run using the style, or a style inheriting from it, is re-styled; their direct formatting stays. One undo step (undo also restores the old definition). Errors (`unknown style property "x"`, a parent cycle, a paragraph property on a character style, …) change nothing. |
| `styleNames(family)` | `string[]` | `"paragraph"` or `"character"`, in definition order. |
| `styleProperties(name, resolved?)` | object | The style's own properties plus `family`, `parent`, `nextStyle`; with `resolved` the values its whole chain gives. `{}` for an unknown name. |
| `applyParagraphStyle(name)` | boolean | On every paragraph the selection touches (the caret's without one). `false` for an unknown or character style. |
| `applyCharStyle(name)` | boolean | On the selection (the word at the caret without one); `""` removes the character style. |
| `clearDirectFormatting()` | | The selection's runs and paragraphs back to exactly what their styles give; the caret's paragraph without a selection. |
| `directFormattingAt(pos?)` | object | `{ paragraphStyle, charStyle, paragraph: {…}, character: {…} }` — the properties that differ from the styles at `pos` (default: the caret). `null` marks a property the style sets and the text removes. |

Style properties (all optional):

| Property | Value | |
|---|---|---|
| `parent` | style name | Inherit what this style does not set. Paragraph styles without one inherit Default Paragraph Style. |
| `nextStyle` | style name | Paragraph styles: the style of the paragraph Enter creates at the end of this one (default: the same). |
| `fontFamily` | string | |
| `fontSize` | number (pt) or `"12pt"` | |
| `fontWeight` | 1–1000, `"bold"`, `"normal"` | |
| `italic`, `underline`, `strike` | boolean | |
| `color` | `"#rrggbb"` | |
| `letterSpacing` | px, or a length string | |
| `marginTop`, `marginBottom`, `marginLeft`, `marginRight`, `textIndent` | px, or a length string (`"1cm"`, `"0.5in"`, `"6pt"`) | Paragraph styles. |
| `lineHeight` | a multiple (`1.5`, `"150%"`) or an exact height (`"18pt"`) | Paragraph styles. |
| `alignment` | `"left"`, `"center"`, `"right"`, `"justify"` | Paragraph styles. |
| `keepWithNext` | boolean | Paragraph styles (kept in the file; print / PDF). |
| `outlineLevel` | 0–10 | Paragraph styles: `n > 0` makes it a heading style (`doc.heading`, `<text:h>` in the file). |

Default Paragraph Style's `fontFamily` and `fontSize` are the document's base font.
ODF imports use that font on screen as well as when saving, including inherited text
inside spans. The base font remains part of the style, rather than direct formatting.
Fresh app-created documents use the editor's font as their display default. The
handler's `fontFamily`/`fontSize` report the effective style value at the caret.

### Starting a document with styles

`newDocument({ styles, paragraphStyle })` sets up a document's style table in one call, instead
of `newDocument()` followed by a `defineStyle()` per style:

```tsx
const r = doc.newDocument({
  styles: {
    "Text Body": { fontSize: 11, lineHeight: 1.4 },
    "Note": { parent: "Text Body", italic: true, nextStyle: "Text Body" },
  },
  paragraphStyle: "Text Body",
});
if (!r.ok) setStatus("Could not start the document: " + r.error);
```

Each entry of `styles` is validated exactly like `defineStyle(name, props)` (same property rules,
the same parent/family/cycle checks); `paragraphStyle` must name a paragraph style (one of the
built-ins, or one just defined) and is what the document's empty first paragraph starts in. Any
error — anywhere in `styles`, or an unknown `paragraphStyle` — changes nothing: the document kept
is whichever one existed before the call, exactly as `loadOdf` leaves it on a failed load. On
success it behaves like the plain `newDocument()` otherwise: no undo step, `modified` stays
`false`, and the styles are written to `styles.xml` on `saveOdf` like any other definition.

## Page look & zoom

The default is print layout: white A4-height sheets on a grey desk, with text flowing across
pages. Sheet items are recycled for the viewport plus one screen above and below. The text
column stays centred in the sheet; wide sheets can be scrolled horizontally.

These additional properties are available in **QML and through the handle** (new TSX attribute
emission is deliberately outside this change):

| Property | Default | Meaning |
| --- | --- | --- |
| `pageHeight` | `1123` | Sheet height in layout pixels at zoom 1. Nonpositive disables pagination. |
| `pageGap` | `24` | Desk space between sheets, in layout pixels at zoom 1; negative values clamp to zero. |
| `pageView` | `pageHeight > 0` | Set `false` for the continuous editor and explicit-break markers. |
| `deskColor` | `"#e9e8e6"` | Background behind and between sheets. |
| `pageCount` | read-only | Reactive number of laid-out pages (at least one). |
| `currentPage` | read-only | Reactive, one-based page containing the caret, independent of scroll position. |

`pageAt(position)` returns the one-based page of a document character position (clamped to the
document). `scrollToPage(n)` clamps the page number and brings the sheet's top to the viewport
top, subject to the scroll range at the end. It does not move the caret. `pagesChanged` is emitted
when the page count changes; use `onPagesChanged` in QML or `doc.pagesChanged.connect(callback)`
from a ref. `currentPage` has its own property-change notification. In continuous view, page
queries return 1 and `scrollToPage` does nothing. Existing `scrollToPosition`, outline, marks,
selection and clipboard methods keep document character positions.

An app's status label can bind directly to the editor:

```qml
Text { text: "Página " + editor.currentPage + " de " + editor.pageCount }
```

Qt's existing `QTextDocumentLayout` owns pagination; no custom document layout is installed.
Its page height is `(pageHeight + pageGap) * zoom`, with the gap added to each root-frame bottom
margin. Paragraphs and table rows can span sheets: their *bounding rectangles* may cover a gap,
but their individual text lines stay inside page text areas. Explicit breaks land at the next
text top, subject to subpixel font rounding and the paragraph's own spacing. Clicking a gap uses
Qt's nearest-text hit testing. An inline image taller than the text area is indivisible: Qt allows
it to overflow through the bottom margin/gap, rather than splitting or shrinking it. Very large
unbreakable content or margins that leave no text area have the same Qt layout limitations.
Page counts reflect this editor's fonts and supported ODF content, not LibreOffice's page count.
Headers, footers, printed page numbers and PDF export are not part of this feature.

`pageWidth`/`pageHeight`/`pageGap`/`pageMargins` are view geometry, do not modify document content,
and do not add undo steps or change saved ODF page setup. `zoom` does: `setZoom()` walks every block and run already in the
live document and multiplies every absolute length baked into it (font point size, letter
spacing, margins/indent, an exact line height, inline image width/height) by the new/old ratio,
in place — the same mechanism `defineStyle`/`applyParagraphStyle` use to re-apply a style, not a
`scale: transform` (so hinting/antialiasing stay correct at any zoom). The named styles
(`m_sheet`) themselves are never touched: `styleProperties()` always reads canonical, zoom-1
values, and `saveOdf`/`saveOdfCopy` divide every length they read off the live document by the
zoom in effect before writing it out — the file is byte-for-byte the same at any zoom. Loading a
file or starting a new document always (re)builds at zoom 1 first, then is brought up to the
zoom already in effect, without becoming an undo step; a zoom change through `zoom` itself is one
ordinary undo step, like any other formatting change.

## Outline

`headings()` is kept **incrementally**, not rescanned per keystroke: the engine listens to
`QTextDocument::contentsChange(position, charsRemoved, charsAdded)` — the same hook a syntax
highlighter uses, fired after an edit's content change but before layout — and re-scans only the
blocks in `[position, position + charsAdded]` for their current heading level, splicing the
result back into the kept outline at the same spot (the blocks a single edit touches are tracked
by their own stable `QTextBlock` handle, not by position, so edits elsewhere never invalidate
them). A single keystroke costs O(1 block); a large paste costs O(blocks pasted); nothing costs
O(whole document) except the first scan when a document is attached, loaded or replaced by
`newDocument()` — all infrequent, not per-keystroke. `currentHeadingIndex` is O(heading count) (a
scan of the kept outline, not the document) on every caret move. `headingsChanged` diffs the
outline (size, then each entry's position/level/text) before firing, so a plain content edit that
touches no heading does not retrigger it.

## Clipboard

`copy()`/`cut()` put both HTML and plain text on the system clipboard; `paste()` prefers the
HTML (so formatting survives a round trip through this or another app) and falls back to plain
text. `canPaste` tracks the clipboard, not this document, so it also updates when another app
copies something.

Zoom (★2) and the clipboard boundary (office item 2b, ★2 follow-up): every length baked into the
live document is at the zoom in effect, but anything crossing the document's boundary is not
automatically at that zoom, so both directions are handled explicitly:

- **Entry** (`paste()`'s HTML branch, and `insertHtml()` directly): the pasted HTML's own declared
  sizes (font size, margins, …) are canonical, zoom-1 values — exactly like a style's values in
  `m_sheet`. The inserted range is rescaled UP to the zoom in effect right after insertion, the
  same way typed text already is, before the edit block closes (one undo step). A new paragraph
  the paste creates gets its whole block format (margins, line height) scaled; content that only
  lands inside, or splits, a pre-existing paragraph keeps that paragraph's own already-correctly-
  zoomed block format — only the character runs actually pasted are touched. Plain-text paste has
  no size of its own: it takes the caret's own (already-zoomed) character format, same as typed
  text, so there is nothing to rescale.
- **Exit** (`copy()`/`cut()`): the HTML placed on the clipboard is built from an UNSCALED copy of
  the selection (rescaled DOWN by `1/zoom` into a throwaway document before `toHtml()`), so it
  always reads in the document's canonical sizes — a paste into another app, or back into this
  one at a different zoom, lands at the original size instead of whatever zoom this document
  happened to be at.

Net effect: the clipboard and a saved file are zoom-independent in both directions, at any zoom —
copy at zoom 2, paste at zoom 1 (or vice versa) reproduces the exact source size.

`insertImage()` and `insertTable()` need no equivalent fix: `insertImage` already computes its
cap at the zoom in effect (★2) and the writer unscales it on save; `insertTable`'s format has no
zoom-dependent absolute length (border width and cell padding are fixed constants, column width
is percentage-based) — nothing in it is lossy across a zoom change.

Native entry paths: `RichText.qml`'s text item intercepts Ctrl+V, Shift+Insert, Ctrl+C and Ctrl+X
(the same `Keys.onPressed` mechanism already used for Ctrl+B/I/U) and routes them to
`paste()`/`copy()`/`cut()`, so the platform's own `QQuickTextEdit::paste()`/`copy()`/`cut()` —
which would bypass the rescaling above — never runs. Qt Quick's `TextEdit` has no built-in
drag-and-drop target (unlike a `QTextEdit` widget, it needs an explicit `DropArea`, which this
module does not add), so there is no drag-and-drop bypass to route. Middle-click paste of the X11
primary selection (`QQuickTextControl`'s own `mouseReleaseEvent`, calling
`QQuickTextEdit::insertFromMimeData` directly) is a theoretical residual gap — it requires
`QGuiApplication::clipboard()->supportsSelection()`, which is false under the offscreen test
platform and on this project's Wayland target (no primary-selection protocol in play), so it is
inert here; fixing it for real X11 use would need a C++ event filter on the text item (installed
from `RichTextHandler`) rather than anything QML's `Keys` attached property can intercept.

## Counts

`wordCount`/`charCount` are kept the same way `headings()` is: a running total, adjusted by the
delta of only the blocks an edit actually touched (the SAME `contentsChange` walk updates the
outline and the counts together), never a full re-count of the document. A **word** is a
`QTextBoundaryFinder::Word` segment that starts with a letter or digit (so runs of whitespace or
punctuation between words don't count); **chars** is a block's own text length — the paragraph
separator between blocks is not a character of either paragraph, so it is excluded (an image
counts as one character, its `U+FFFC` placeholder). `selectionWordCount`/`selectionCharCount`/
`selectedText` are computed live, not kept incrementally — they are bounded by the selection's
own size, not the document's, so there is nothing to precompute.

## Blocks and marks

What a live spell or grammar checker, or review highlights, need: a stable handle on each
paragraph, a way to hear which paragraphs changed, and squiggles/highlights drawn over the text
without becoming part of it.

```tsx
const check = (id: string) => {
  doc.clearMarks("spelling", id);
  misspelled(doc.blockText(id))                             // the app's checker
    .forEach((w) => doc.addMark("spelling", id, w.start, w.end));
};
<DocumentEditor ref={doc}
  onBlocksLoaded={() => doc.blockIds().forEach(check)}       // the first full pass
  onBlockChanged={(id) => check(id)}                         // then only what changed
  onBlockRemoved={(id) => forget(id)}
  onMarkContextMenu={(m, x, y, wx, wy) => { setPending(m); menu.open(wx, wy); }} />
// in the menu: doc.replaceRange(pending().blockId, pending().start, pending().end, "the")
```

`examples/document-editor.tsx` does exactly this with a toy rule (every "teh" is misspelled;
right-click offers "the").

For an app's “review the selection” action, read `selectedText` and the reactive endpoints
from the handle. `blockIdsInRange` has an **inclusive** end, whereas selected text excludes
`selectionEnd`. To get only blocks containing selected characters (or the caret's block when
collapsed):

```ts
const from = doc.selectionStart;
const to = Math.max(from, doc.selectionEnd - 1);
const blocksToReview = doc.blockIdsInRange(from, to);
// Dismiss one review annotation without touching other annotations or document history:
doc.removeMark(reviewMarkId);
```

**Block ids.** Every block — paragraph, heading, list item, table-cell paragraph — has a string
id, unique in the document. It stays the same while the block is typed in and while other blocks
are inserted or removed. Enter gives the part **after** the caret a new id (the part before keeps
its own, even when it is empty); a merge (Backspace at the start of a block, Delete at the end of
one) keeps the upper block's id — except when the upper block is empty, then it is the one that
goes and the lower keeps its id. Every block of a paste is new (the clipboard carries no ids, not
even from this editor). Undo and redo give recreated blocks the ids they had: undoing a merge
brings the merged-away paragraph back with its old id; redoing a split re-creates the same new
id. Ids are saved in the file as `xml:id` on `text:p`/`text:h` (ODF 1.2+, read back by our
reader); a file without them (or repeating one) gets generated ids on load. A new document or
a load replaces every id (`onBlocksLoaded`).

How: the id lives in the block's `QTextBlockUserData`, which Qt keeps with the block node itself
(verified against Qt 6.11 `qtextdocument_p.cpp`: a split inserts a NEW node for the text after
the split point; a merge erases the lower node — or the upper one when it is empty — and its user
data is deleted with it, which is how removals are detected exactly). Undo/redo do not restore
user data, so the engine records, per structural edit, the ids it removed and created under the
undo-stack state it ends at, and hands them back when that step is undone/redone
(`QTextDocument::undoCommandAdded` tells a new edit from a replay). An undo through anything
other than the document's own stack (a host calling `QTextDocument::undo` is fine — that IS the
stack) cannot be told apart; at worst a recreated block gets a fresh id.

`onBlockChanged` compares a hash of each edited block's text, so bold, a style, alignment or zoom
never fire it. Detection is incremental: only the blocks an edit touched are looked at.

**Marks** are an overlay, not document content: they are never saved, copied to the clipboard,
put in the undo history, and adding or clearing them does not set `modified`. They are kept as
document positions and follow the text: an edit before a mark shifts it; after it, nothing;
typing exactly at its start shifts it, exactly at its end leaves it as it was (the new character
is not part of it). An edit **inside** a mark — typing into it, deleting part or all of it,
`replaceRange` over it — **removes** it: a spell/grammar result for text that changed is stale,
and the app re-checks the block on `onBlockChanged` anyway. Marks move with their text across
blocks too: Enter before a mark carries it into the new block (`markAt` then reports the new
`blockId` and offsets relative to it); a merge carries the lower block's marks into the upper
one. Formatting never touches them. Undo is an edit like any other (it does not bring marks back).
`loadOdf`/`newDocument` start without marks. Cost: an edit shifts the mark list once — O(marks),
a tight loop — nothing else.

Default looks: `"spelling"` a red wave, `"grammar"` a blue wave, `"highlight"` a yellow
background; any other kind a grey single underline until the app calls `markStyle`. A mark kind
can have both an underline and a background.

Drawing: the marks overlay is a child item of the text area stacked **under** the text (so a
highlight sits behind the glyphs and the selection shows over it) — rectangles for backgrounds and
single underlines, a repeated sine texture for the wave. It is not a `QSyntaxHighlighter`: Qt
Quick's text node engine (Qt 6.11 `qquicktextnodeengine.cpp`, `mergeFormats`) only honours
additional layout formats that carry a foreground, a background or an anchor, and draws underlines
from `QFont::underline()` as straight lines — a `WaveUnderline`/`SpellCheckUnderline` format is not
drawn by `TextEdit` at all, and changing layout formats re-lays-out the block. The overlay follows
the viewport (`ItemObservesViewport`): it only lays out the marks of the visible part plus a
quarter viewport around it, found by a binary search over the (sorted) marks, and is redone when
the view scrolls out of that band, the text changes, or the marks change.

Interaction: clicks and hover come from passive pointer handlers on the text area, so the caret
and selection behave exactly as without marks; the hit test takes the character under the pointer
(not the nearest caret position).

## Notes

- Name your component's signals so they don't collide with the handle's properties (`modified`,
  `bold`, `heading`, …) when the editor is the component's root element — a signal becomes a
  property on the root, and two properties of the same name shadow each other. The same trap
  bites an *attribute* binding too: `<DocumentEditor zoom={zoom()}>` with a signal also named
  `zoom` emits `zoom: zoom`, which QML resolves against the element's own property first — a
  self-reference, not the outer signal. Name the signal something else (`zoomLevel`, as
  `examples/document-editor.tsx` does) when it shares a name with `pageWidth`/`pageMargins`/
  `zoom` too.
- ODF coverage is the word-like subset: paragraphs and headings (`text:h` with its outline
  level), spans (nested ones too), bullet and numbered lists (nested by level), tables (merged
  cells), inline images, a manual page break before a paragraph (`fo:break-before="page"`, from
  `insertPageBreak()`), and the named paragraph/character styles of `styles.xml` with the
  automatic styles over them as direct formatting. Our writer produces `content.xml`,
  `styles.xml` (the named styles), `meta.xml` and the manifest — LibreOffice 26.8 opens the result
  with every named style, parent, next style and heading level in place. Not kept yet: page SETUP
  (size/margins/printing) and headers/footers, footnotes as footnotes (their text is kept,
  inline), bookmarks and links (their text is kept), unknown package entries. A paragraph whose
  styles give it no vertical
  spacing gets a quarter pixel under it (invisible; it keeps Qt's viewport culling from
  over-drawing). Files saved by the earlier Qt-based writer still open (their headings come back
  as direct formatting on Default Paragraph Style).
- Large documents (> 10 000 characters, however they got there: a load, typing, a paste) render
  only the part near the viewport, as Qt's `TextEdit` does for a large `text`.

## Page layout engine

`layoutEngine` is a runtime QML/ref property: `"auto"` (default), `"qt"`, or
`"custom"`. In page view, auto/custom select the custom layout for supported
live documents, including flat, in-flow tables with solid borders. Continuous view,
nested tables, repeated table headers, other child frames, root
borders/padding/backgrounds, hidden blocks or horizontal rules use Qt. Forcing custom does not bypass that
eligibility check. Changing engines preserves the document, resources and undo
history; it does not round-trip through HTML.

HTML insertion is inspected before the selection or document is edited. Unsupported
content switches the whole document to Qt, retaining that engine through undo/redo
so replay cannot restore an unsupported structure into the custom engine. A new
or newly loaded supported document can use the custom engine again. Table borders
and backgrounds use a public Qt Quick decoration item behind the text, restricted
to visible page fragments. Native code that edits the exposed
`QTextDocument` with structural `QTextCursor` calls must select `"qt"` before
unsupported calls; `insertTable`, paste and load through the handler check eligibility
before editing.

### Differences from Qt's layout

The phase-2 acceptance policy top-aligns cells in **every fragment of a split
row**. Middle/bottom alignment applies only when the row fits on one page.
This makes placement deterministic when an edit changes where a row splits and
prevents alignment from moving ordinary text into a page margin or gap. The
stored cell vertical-alignment property must still survive ODF round trips.

`tests/table_phase2_test.cpp` records the Qt bug motivating this policy: with
eight ordinary lines in one cell, Qt places the middle-aligned `X` in the other
cell at y=115, painting into the gap at y=120–140. Its
`TABLE_REQUIRE_CLEAR_GAPS` mode exercises the custom engine on the same fixture.
This policy does not waive Qt parity for non-split rows.

## Qt private API used

The following compatibility boundaries rely on Qt internals and must be checked when
upgrading Qt:

- `rt::setRootMarginsNoUndo` in `src/shims/richtext.cpp` uses
  `QTextDocumentPrivate::formatCollection()` to change the root frame's view margins.
  The public `QTextFrame::setFrameFormat` records an undo step, and toggling
  `setUndoRedoEnabled` clears the undo/redo stack. The caller suspends layout while
  applying margins and page size together. This helper preserves document history.
- `rt::refreshTextEditSize` invokes QQuickTextEdit's private `updateSize()` slot by
  name through `QMetaObject::invokeMethod`. Public `contentHeight` is read-only;
  after suspended loading it can retain the previous document's size, including
  when page count is unchanged. A missing slot produces a warning and returns
  false rather than requiring its private header or crashing.
- `rt::attachCustomLayoutToTextEdit` in `src/shims/richtext_qt_attach.cpp`
  reconnects `q_contentsChange`, `invalidateBlock`, `moveCursorDelegate` and
  `updateSize` by meta-object name, and forwards the public undo/redo notify signals.
  It relies on non-public slot **names**, without private headers. It validates
  every method and connection, disconnects partial connections on failure, and
  returns false with a warning so the caller can retain or restore Qt before editing.
  The in-place route retains the existing document links; replacement documents
  need those links restored. Layout destruction removes the helper's connections.
- `RichTextHandler::mergeInsertionFont` accesses the live cursor through
  `QQuickTextEditPrivate` and `QQuickTextControlPrivate`. The public TextEdit API
  does not expose insertion-only character formatting, and TextControl methods
  are not exported by release Qt. This keeps typing and IME insertion consistent
  without formatting the word under the caret.
- `RichTextHandler::refreshViewport` uses `QQuickTextEditPrivate` to request a
  complete text-node update after backward viewport movement. A public item
  update alone does not reset TextEdit's cached traversal state, which can leave
  newly visible lines unpainted. This is the existing viewport-culling repair.

Tests also inspect QQuickTextEdit's private rendered region to check viewport
culling; that inspection is not a production dependency.

### Document page geometry

`documentPageWidth`, `documentPageHeight`, and `documentPageMargins` expose the
first body paragraph's ODF master-page geometry in px at 96 dpi. Margins are
`[top, right, bottom, left]`, matching `pageMargins`. The reader follows named
and automatic paragraph-style parents to the master page, falling back to the
first master when none is assigned. A document without a supported page layout
reports `0`, `0`, and `[]`. `newDocument()` clears the metadata; a failed load
that leaves the document untouched preserves it. These are read-only properties;
`documentPageGeometryChanged` fires only when their values change.

After a successful load, applications can assign these values to `pageWidth`,
`pageHeight`, and `pageMargins`. This keeps application display settings explicit.
Mixed page masters within one document are not yet supported by pagination.
