// M-AOT-1 fixture: <img src> → SolidWidgets::Image (sized via CSS so it actually rasterizes).
// Absolute src so QML and AOT resolve identically.
import { div, img, Window } from "../../src/solid-qml/runtime";
import "./image.css";

function ImageDemo() {
  return (
    <div class="app">
      <img class="logo" src="/home/caetano/lab/solid-qml-native/assets/logo.png" />
    </div>
  );
}
export function ImageApp() {
  return <Window title="Image" width={200} height={180} visible><ImageDemo /></Window>;
}
