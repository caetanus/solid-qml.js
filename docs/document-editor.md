# Document editor

`<DocumentEditor>` is the editing surface of a word processor: the **app** drives the document —
opens and saves it, draws its own toolbar from the format state, and reacts to edits. Body text
and headings are **named styles** (LibreOffice's: "Text Body", "Heading 1", …), not direct
formatting. It shares its engine with `<RichText>` (module `solidqml.Widgets.RichText`: a Qt
`TextArea` plus the loader's `RichTextHandler`, with our own OpenDocument reader and writer).
`<RichText>` stays as it is for mail and forms (`html` in, `onChange(html, text)` out).

It is native-only (no browser equivalent); the module is imported only when the tag is used.

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
| `pageWidth` | number (px) | The editor's implicit width when set (e.g. `794` ≈ A4 at 96dpi). `0`/unset keeps the plain editor's default. Continuous — no on-screen page breaks (see `insertPageBreak()` for the content-level marker that round-trips to the file). |
| `pageMargins` | `[top, right, bottom, left]` (px), or `null`/unset | Applied as the editing surface's padding, so the caret and selection respect them. `null`/unset keeps the plain editor's default (12px uniform). |
| `zoom` | number, clamped `0.5`–`3.0` | A **real relayout**: font/point sizes, margins and inline images are rebuilt at the new size (not a scale transform — text stays sharp). Does not change what `saveOdf` writes. The app owns the value; see `onZoomRequested`. |
| `onZoomRequested` | `(delta: number) => …` | Ctrl+wheel (±0.1 per notch), Ctrl+= / Ctrl+- (±0.1), Ctrl+0 (`0`, meaning "reset"). The editor only asks; set `zoom` in response (e.g. `setZoom(z => delta === 0 ? 1 : clamp(z + delta))`). |
| `onHeadingsChanged` | `() => …` | The outline changed, **debounced** (300 ms) and only when `doc.headings()` actually differs from before. Refresh a table of contents from `doc.headings()` here. |

The editor starts **empty and unmodified** (no demo text). Anything else is a transpile error.

## The handle (`ref`)

State — reactive: reading `doc.bold` in a binding (a `classList`, a text) follows the caret.

| Property | Type | |
|---|---|---|
| `modified` | boolean | Unsaved edits. Cleared by `loadOdf`, `saveOdf`, `newDocument` (not by `saveOdfCopy`). |
| `bold`, `italic`, `underline`, `strike` | boolean | Character format at the caret / selection. |
| `heading` | number | `0` not a heading, `1`–`6` heading (outline) level. |
| `fontFamily` | string | Character format at the caret / selection; `""` when unset (Default Paragraph Style carries no explicit font by design — the editor's own font stands in). |
| `fontSize` | number | pt, in canonical (zoom-1) units — like a word processor's font-size box, the same number at any zoom; `0` when unset. |
| `currentParagraphStyle` | string | The caret's paragraph style (`"Text Body"`, `"Heading 1"`, …). |
| `currentCharStyle` | string | The character style at the caret, `""` for none. |
| `alignment` | `"left" \| "center" \| "right" \| "justify"` | Paragraph alignment. |
| `bulletList`, `numberedList`, `inTable` | boolean | The caret is in a bullet / numbered list (mutually exclusive) / a table. |
| `canUndo`, `canRedo` | boolean | |
| `canPaste` | boolean | The SYSTEM clipboard (not this document) currently carries pasteable content — tracks another app too. |
| `wordCount`, `charCount` | number | Whole document, kept incrementally (see "Counts" below) and notified (`contentsChanged`) after `loadOdf`/`newDocument`/attaching a document, not just a keystroke. A word is a Unicode word-boundary segment starting with a letter or digit; chars excludes the paragraph separator between blocks. |
| `selectionWordCount`, `selectionCharCount` | number | The current selection, by the same rules; `0` with none. |
| `selectedText` | string | `""` with no selection; paragraphs separated by `"\n"`, like `plainText()`. |
| `currentHeadingIndex` | number | Index into `headings()` of the heading at/before the caret, `-1` if none. |

Methods:

| Method | Returns | |
|---|---|---|
| `headings()` | `{position, level, text}[]` | The outline, in document order. Kept incrementally by the engine (see "Outline" below) — cheap to call even on a very large document. |
| `scrollToPosition(pos)` | | Scrolls so `pos` sits near the top of the viewport (e.g. `doc.scrollToPosition(doc.headings()[i].position)`). |
| `loadOdf(path)` | `{ ok, error }` | Opens an `.odt`, with its named styles. On failure (`file not found: …`, `not an OpenDocument file: …`) the current document is kept. The load is not an undo step. |
| `saveOdf(path)` | `{ ok, error }` | Writes an `.odt` (our writer: named styles, heading levels, images embedded; atomic — a failed write leaves the old file). Clears `modified` on success. |
| `saveOdfCopy(path)` | `{ ok, error }` | The same `.odt`, for autosave / crash recovery: the document is left as it was — `modified` stays, the undo history is untouched. |
| `newDocument(options?)` | `{ ok, error }` | Empty, unmodified document in Default Paragraph Style; drops undo history, images and style definitions (back to the built-in styles). With `options`, styles are defined first (see "Starting a document with styles" below) and `{ ok, error }` reports it, like `loadOdf`. |
| `toHtml()` / `plainText()` | string | The content; `plainText()` separates paragraphs with `\n`. |
| `insertText(text)` | | At the caret (replacing a selection); `\n` starts a paragraph, as Enter does (next style included). |
| `toggleBold()`, `toggleItalic()`, `toggleUnderline()`, `toggleStrike()` | | On the selection; with nothing selected, on what is typed next (as the toolbar does). |
| `setHeading(level)` | | `applyParagraphStyle("Heading " + level)`; `0` → `"Text Body"`. |
| `setFontFamily(name)`, `setFontSize(pt)` | | On the selection; with nothing selected, on what is typed next (same mechanics as `toggleBold`). `pt` is canonical (zoom-1) units, applied at the zoom in effect. |
| `clearFormatting()` | | Drops direct CHARACTER formatting (bold, font, size, …) in the selection; keeps character styles. Unlike `clearDirectFormatting()`, paragraph properties (alignment, margins, …) are untouched. |
| `setAlignment(align)` | | On every paragraph the selection touches. |
| `toggleBulletList()`, `toggleNumberedList()` | | Each mirrors the other exactly: in ANY list, detach it (plain paragraph); otherwise start one of its own kind. Toggling the other kind while already in a list turns the list off, not a conversion. |
| `indent()`, `outdent()` | | In a list: one level deeper/shallower (`outdent()` past the first level detaches it). Otherwise: the paragraph's own left margin, one ~24px step, at the zoom in effect. |
| `insertPageBreak()` | | Splits the paragraph at the caret (`insertParagraph()`'s mechanics) and marks the new one to start on a fresh page. No on-screen effect — the editor is continuous (★1) — but round-trips through `saveOdf`/`loadOdf` as `fo:break-before="page"`. |
| `insertTable(rows, cols)`, `addTableRow()`, `addTableColumn()`, `removeTableRow()`, `removeTableColumn()` | | Row/column edits act on the cell at the caret. |
| `insertImage(path)` | `{ ok, error }` | Inline image at the caret, capped to the column width. |
| `undo()`, `redo()`, `focusEditor()` | | |
| `cut()`, `copy()`, `paste()` | | HTML (formatting kept, rescaled to canonical zoom-1 sizes — see "Clipboard" below) and plain text on the SYSTEM clipboard; `cut()`/`copy()` are no-ops without a selection, `paste()` when `!canPaste`. Ctrl+C/X/V are routed to these (not the editor's own built-in handling) — these are also there for an app's own toolbar/menu. |
| `insertHtml(html)` | | At the caret (replacing a selection), rescaled to the zoom in effect — the same choke point `paste()` uses for its HTML branch. |

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

Default Paragraph Style's `fontFamily` and `fontSize` are the document's base font: kept and
saved (a LibreOffice file's 12pt Times stays 12pt Times there), but on screen the editor's own
font stands in for them, so a file's base font never overrides the app's typography. Every other
style's font shows as defined.

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

`pageWidth`/`pageMargins` only size the editing surface and its padding — they do not change any
character or paragraph format. `zoom` does: `setZoom()` walks every block and run already in the
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
- Marks (spell-check squiggles, review highlights) are the next item on the office roadmap.
- Large documents (> 10 000 characters, however they got there: a load, typing, a paste) render
  only the part near the viewport, as Qt's `TextEdit` does for a large `text`.
