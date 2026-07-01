// Web entry: render the same <Gallery> component the native entry (mainqml.tsx) wraps in a Window.
// Web is the reference; the QML render replicates it. (The old welcome screen is gone — the gallery
// composes the example components directly, no iframe.)
import { render } from "solid-js/web";
import { Gallery } from "../examples/gallery";

render(() => <Gallery />, document.getElementById("root")!);
