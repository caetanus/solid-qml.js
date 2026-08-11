// M-AOT-1 fixture: props.children slotting — a wrapper component renders whatever it's given.
import type { ParentProps } from "solid-js";
import { div, text, Window } from "../../src/solid-qml/runtime";
import "../examples.css";

function Card(props: ParentProps) {
  return <div class="card">{props.children}</div>;
}
function ChildrenDemo() {
  return (
    <div class="app">
      <Card>
        <text class="h1">Inside the card</text>
        <text class="bio">slotted via props.children</text>
      </Card>
    </div>
  );
}
export function ChildrenApp() {
  return <Window title="Children" width={420} height={220} visible><ChildrenDemo /></Window>;
}
