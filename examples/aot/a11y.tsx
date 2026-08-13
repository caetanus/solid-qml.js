import { div, text, h1, img, button, Window } from "../../src/solid-qml/runtime";
import "../examples.css";
function A11yDemo() {
  return (
    <div class="app">
      <h1 class="h1">Heading here</h1>
      <text class="bio">plain run</text>
      <img class="logo" src="/home/caetano/lab/solid-qml-native/assets/logo.png" alt="the Q logo" />
      <button class="bio">Press me</button>
    </div>
  );
}
export function A11yApp() {
  return <Window title="a11y" width={420} height={220} visible><A11yDemo /></Window>;
}
