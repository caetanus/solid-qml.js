// Native-only widget showcase (plan phases 2–3). Guarded by the Electron-idiom probe:
// the browser build renders the fallback card — every tag below is QML-only surface.
import { Show } from "solid-js";
import { div, text } from "../src/solid-qml/runtime";
import { HtmlWidgets } from "./native/htmlwidgets";
import { Containers } from "./native/containers";
import { ExtraInputs } from "./native/inputs";
import { MenusAndViews } from "./native/menus";
import "./native.css";

export function NativeOnly() {
  const isNative = typeof process !== "undefined" && !!(process.versions && process.versions.solidQml);
  return (
    <div class="native">
      <Show
        when={isNative}
        fallback={
          <div class="native-fallback">
            <text class="native-fallback-t">native-only widgets — run this view in the QML loader</text>
          </div>
        }
      >
        <HtmlWidgets />
        <Containers />
        <ExtraInputs />
        <MenusAndViews />
      </Show>
    </div>
  );
}
