// <DocumentEditor>: the app drives the document (a word processor's editing surface). The built-in
// toolbar is hidden; this component draws its own, reading the format state at the caret through
// the ref (doc.bold, doc.heading, …) and calling the editor's operations. File IO returns
// {ok, error}; `modified` drives the title dot; the debounced onContentChanged keeps a word count.
// Native-only (QML surface): run it with examples/document-editor.qml.tsx. See docs/document-editor.md.
import { createSignal } from "solid-js";
import { div, text } from "../src/solid-qml/runtime";
import "./document-editor.css";

export function DocumentEditorDemo() {
  let doc!: DocumentEditorHandle;
  const [path, setPath] = createSignal("document.odt");
  const [dirty, setDirty] = createSignal(false);
  const [words, setWords] = createSignal(0);
  const [status, setStatus] = createSignal("New document");

  const countWords = () => {
    const parts = doc.plainText().split("\n").join(" ").split(" ");
    setWords(parts.filter((w) => w.length > 0).length);
  };
  const open = () => {
    const r = doc.loadOdf(path());
    setStatus(r.ok ? "Opened " + path() : "Open failed: " + r.error);
  };
  const save = () => {
    const r = doc.saveOdf(path());
    setStatus(r.ok ? "Saved " + path() : "Save failed: " + r.error);
  };
  const fresh = () => {
    doc.newDocument();
    setStatus("New document");
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
        <button class="de-btn" onClick={() => doc.insertTable(2, 2)}>Table</button>
      </div>
      <div class="de-page">
        <DocumentEditor ref={doc} class="de-editor" toolbar={false}
          onModifiedChanged={(m) => setDirty(m)}
          onContentChanged={() => countWords()} />
      </div>
      <div class="de-status">
        <text class="de-status-t">{(dirty() ? "● " : "") + status()}</text>
        <text class="de-status-t">{words() + " words"}</text>
      </div>
    </div>
  );
}
