// <DocumentEditor>: the app drives the document (a word processor's editing surface). The built-in
// toolbar is hidden; this component draws its own, reading the format state at the caret through
// the ref (doc.bold, doc.heading, …) and calling the editor's operations. File IO returns
// {ok, error}; `modified` drives the title dot; the debounced onContentChanged keeps a word count.
// Marks (office item 3): a toy spell check marks every "teh"; right-click one to replace it.
// Native-only (QML surface): run it with examples/document-editor.qml.tsx. See docs/document-editor.md.
import { createSignal } from "solid-js";
import { div, text } from "../src/solid-qml/runtime";
import "./document-editor.css";

export function DocumentEditorDemo() {
  let doc!: DocumentEditorHandle;
  const [path, setPath] = createSignal("document.odt");
  const [dirty, setDirty] = createSignal(false);
  const [status, setStatus] = createSignal("New document");
  const [headingCount, setHeadingCount] = createSignal(0);
  // Named zoomLevel, not zoom: a signal with the SAME name as a native prop it is bound to
  // shadows itself in the emitted QML (`zoom: zoom` becomes self-referential — see
  // docs/document-editor.md's naming note).
  const [zoomLevel, setZoomLevel] = createSignal(1.0);
  const applyZoomDelta = (delta: number) =>
    setZoomLevel(delta === 0 ? 1.0 : Math.max(0.5, Math.min(3.0, zoomLevel() + delta)));

  // doc.wordCount/charCount (item 7) are kept incrementally by the engine — no app-side counting.
  const refreshOutline = () => setHeadingCount(doc.headings().length);
  const open = () => {
    const r = doc.loadOdf(path());
    setStatus(r.ok ? "Opened " + path() : "Open failed: " + r.error);
  };
  const save = () => {
    const r = doc.saveOdf(path());
    setStatus(r.ok ? "Saved " + path() : "Save failed: " + r.error);
  };
  const fresh = () => {
    const r = doc.newDocument();
    setStatus(r.ok ? "New document" : "New document failed: " + r.error);
  };

  // A toy spell check: every "teh" is a spelling mark — the whole document on a load (onBlocksLoaded),
  // then only the paragraph that changed (onBlockChanged, coalesced by the editor). Marks are an
  // overlay: they never reach the file, the clipboard or the undo history.
  let fixMenu: any;
  const [fixing, setFixing] = createSignal<DocumentMark | null>(null);
  const checkBlock = (id: string) => {
    doc.clearMarks("spelling", id);
    // Offsets are UTF-16 positions in the paragraph's text: each "teh" ends where its part does.
    doc.blockText(id).split("teh").slice(0, -1).reduce((at, part) => {
      doc.addMark("spelling", id, at + part.length, at + part.length + 3);
      return at + part.length + 3;
    }, 0);
  };
  const checkAll = () => doc.blockIds().forEach((id) => checkBlock(id));
  // Opened synchronously inside the right-click (a Wayland popup needs the fresh input serial), at
  // the click's window coordinates (what a menu's open(x, y) takes).
  const offerFix = (mark: DocumentMark, x: number, y: number) => {
    setFixing(mark);
    fixMenu.open(x, y);
  };
  const applyFix = () => {
    const m = fixing();
    if (m) doc.replaceRange(m.blockId, m.start, m.end, "the");
    setFixing(null);
  };

  return (
    <div class="de">
      <div class="de-bar">
        <button class="de-btn" onClick={fresh}>New</button>
        <button class="de-btn" onClick={open}>Open</button>
        <button class="de-btn" onClick={save}>Save</button>
        <input class="de-path" value={path()} onInput={(e) => setPath(e.currentTarget.value)} />
        <div class="de-sep" />
        <button class="de-btn" classList={{ on: doc.bold }} onClick={() => doc.toggleBold()}>B</button>
        <button class="de-btn" classList={{ on: doc.italic }} onClick={() => doc.toggleItalic()}>I</button>
        <button class="de-btn" classList={{ on: doc.underline }} onClick={() => doc.toggleUnderline()}>U</button>
        <button class="de-btn" classList={{ on: doc.heading === 0 }} onClick={() => doc.setHeading(0)}>Body</button>
        <button class="de-btn" classList={{ on: doc.heading === 1 }} onClick={() => doc.setHeading(1)}>H1</button>
        <button class="de-btn" classList={{ on: doc.heading === 2 }} onClick={() => doc.setHeading(2)}>H2</button>
        <button class="de-btn" classList={{ on: doc.alignment === "center" }} onClick={() => doc.setAlignment(doc.alignment === "center" ? "left" : "center")}>Center</button>
        <button class="de-btn" classList={{ on: doc.bulletList }} onClick={() => doc.toggleBulletList()}>List</button>
        <button class="de-btn" classList={{ on: doc.numberedList }} onClick={() => doc.toggleNumberedList()}>1.</button>
        <button class="de-btn" onClick={() => doc.outdent()}>⇤</button>
        <button class="de-btn" onClick={() => doc.indent()}>⇥</button>
        <button class="de-btn" onClick={() => doc.insertTable(2, 2)}>Table</button>
        <button class="de-btn" onClick={() => doc.insertPageBreak()}>Page break</button>
        <div class="de-sep" />
        <button class="de-btn" onClick={() => doc.cut()}>Cut</button>
        <button class="de-btn" onClick={() => doc.copy()}>Copy</button>
        <button class="de-btn" classList={{ off: !doc.canPaste }} onClick={() => doc.paste()}>Paste</button>
        <div class="de-sep" />
        <button class="de-btn" onClick={() => applyZoomDelta(-0.1)}>−</button>
        <text class="de-status-t">{Math.round(zoomLevel() * 100) + "%"}</text>
        <button class="de-btn" onClick={() => applyZoomDelta(0.1)}>+</button>
        <button class="de-btn" onClick={() => applyZoomDelta(0)}>100%</button>
      </div>
      <div class="de-page">
        <DocumentEditor ref={doc} class="de-editor" toolbar={false}
          pageWidth={794} pageMargins={[48, 48, 48, 48]} zoom={zoomLevel()}
          onZoomRequested={(delta) => applyZoomDelta(delta)}
          onModifiedChanged={(m) => setDirty(m)}
          onContentChanged={() => {}}
          onHeadingsChanged={() => refreshOutline()}
          onBlocksLoaded={() => checkAll()}
          onBlockChanged={(id) => checkBlock(id)}
          onMarkContextMenu={(m, x, y, wx, wy) => offerFix(m, wx, wy)} />
        <Menu ref={fixMenu}>
          <MenuItem onClick={() => applyFix()}>the</MenuItem>
        </Menu>
      </div>
      <div class="de-status">
        <text class="de-status-t">{(dirty() ? "● " : "") + status()}</text>
        <text class="de-status-t">{doc.wordCount + " words, " + doc.charCount + " chars"}</text>
        <text class="de-status-t">{headingCount() + " headings"}</text>
      </div>
    </div>
  );
}
