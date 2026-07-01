// Prop helpers: mergeProps (defaults) + splitProps (named). Plain component (no <Window>/render).
import { mergeProps, splitProps } from "solid-js";
import { div, text } from "../src/solid-qml/runtime";

function Greeting(props) {
  const merged = mergeProps({ greeting: "Hello", name: "stranger" }, props);
  return <text class="h1">{merged.greeting}, {merged.name}!</text>;
}

function Tag(props) {
  const [local] = splitProps(props, ["label"]);
  return <text class="bio">#{local.label}</text>;
}

export function PropHelpers() {
  return (
    <div class="app">
      <Greeting />
      <Greeting name="Solid" />
      <Greeting greeting="Hi" name="QML" />
      <Tag label="native" />
    </div>
  );
}
