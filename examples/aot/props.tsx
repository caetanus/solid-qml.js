// M-AOT-1 fixture: prop helpers — mergeProps (defaults) + splitProps (named). Self-contained.
import { mergeProps, splitProps } from "solid-js";
import { div, text, Window } from "../../src/solid-qml/runtime";
import "../examples.css";

function Greeting(props: { greeting?: string; name?: string }) {
  const merged = mergeProps({ greeting: "Hello", name: "stranger" }, props);
  return <text class="h1">{merged.greeting}, {merged.name}!</text>;
}
function Tag(props: { label: string }) {
  const [local] = splitProps(props, ["label"]);
  return <text class="bio">#{local.label}</text>;
}
function PropHelpers() {
  return (
    <div class="app">
      <Greeting />
      <Greeting name="Solid" />
      <Greeting greeting="Hi" name="QML" />
      <Tag label="native" />
    </div>
  );
}
export function PropsApp() {
  return <Window title="Props" width={420} height={220} visible><PropHelpers /></Window>;
}
