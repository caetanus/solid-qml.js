# Document editor

`<DocumentEditor>` is the editing surface of a word processor: the **app** drives the document —
opens and saves it, draws its own toolbar from the format state, and reacts to edits. It shares
its engine with `<RichText>` (module `solidqml.Widgets.RichText`: a Qt `TextArea` plus the
loader's `RichTextHandler`, OpenDocument saved by Qt's writer and read by our `content.xml`
reader). `<RichText>` stays as it is for mail and forms (`html` in, `onChange(html, text)` out).

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
        <button classList={{ on: doc.heading === 1 }} onClick={() => doc.setHeading(1)}>H1</button>
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
| `onContentChanged` | `() => …` | Any edit, formatting included, **debounced** (300 ms of quiet). No payload: read `doc.plainText()` / `doc.toHtml()` when needed. Opening a file and `newDocument()` count as content changes. |

The editor starts **empty and unmodified** (no demo text). Anything else is a transpile error.

## The handle (`ref`)

State — reactive: reading `doc.bold` in a binding (a `classList`, a text) follows the caret.

| Property | Type | |
|---|---|---|
| `modified` | boolean | Unsaved edits. Cleared by `loadOdf`, `saveOdf`, `newDocument`. |
| `bold`, `italic`, `underline`, `strike` | boolean | Character format at the caret / selection. |
| `heading` | number | `0` body text, `1`–`6` heading level. |
| `alignment` | `"left" \| "center" \| "right" \| "justify"` | Paragraph alignment. |
| `bulletList`, `inTable` | boolean | The caret is in a bullet list / a table. |
| `canUndo`, `canRedo` | boolean | |

Methods:

| Method | Returns | |
|---|---|---|
| `loadOdf(path)` | `{ ok, error }` | Opens an `.odt`. On failure (`file not found: …`, `not an OpenDocument file: …`) the current document is kept. The load is not an undo step. |
| `saveOdf(path)` | `{ ok, error }` | Writes an `.odt` (Qt's native writer, images embedded). Clears `modified` on success. |
| `newDocument()` | | Empty, unmodified document; drops undo history and images. |
| `toHtml()` / `plainText()` | string | The content; `plainText()` separates paragraphs with `\n`. |
| `insertText(text)` | | At the caret (replacing a selection); `\n` starts a paragraph. |
| `toggleBold()`, `toggleItalic()`, `toggleUnderline()`, `toggleStrike()` | | On the selection; with nothing selected, on what is typed next (as the toolbar does). |
| `setHeading(level)` | | `0` body, `1`–`3` sized headings (`4`–`6` bold). |
| `setAlignment(align)` | | On every paragraph the selection touches. |
| `toggleBulletList()` | | |
| `insertTable(rows, cols)`, `addTableRow()`, `addTableColumn()`, `removeTableRow()`, `removeTableColumn()` | | Row/column edits act on the cell at the caret. |
| `insertImage(path)` | `{ ok, error }` | Inline image at the caret, capped to the column width. |
| `undo()`, `redo()`, `focusEditor()` | | |

Formatting calls give the keyboard back to the editor, so an app toolbar button does not leave
the caret stranded. Ctrl+B / Ctrl+I / Ctrl+U work with or without the built-in toolbar.

## Notes

- Name your component's signals so they don't collide with the handle's properties (`modified`,
  `bold`, `heading`, …) when the editor is the component's root element — a signal becomes a
  property on the root, and two properties of the same name shadow each other.
- ODF coverage is the word-like subset: paragraphs, headings, bold/italic/underline/strike spans,
  flat bullet lists, tables, inline images. Paragraph spacing, side margins, first-line indent and
  alignment are read from the paragraph styles — automatic ones and the named ones in `styles.xml`
  they inherit from (`Text_20_body`, `Heading`, …); a paragraph whose styles give it no vertical
  spacing gets a quarter pixel under it (invisible; it keeps Qt's viewport culling from
  over-drawing). Named styles as styles (kept on save, character styles) and marks (spell-check
  squiggles, review highlights) are the next items on the office roadmap.
- Large documents (> 10 000 characters, however they got there: a load, typing, a paste) render
  only the part near the viewport, as Qt's `TextEdit` does for a large `text`.
