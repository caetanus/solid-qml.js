// props.children: a wrapper component slots whatever it's given (no <Window>/render).
import type { ParentProps } from "solid-js";
import { div, text } from "../src/solid-qml/runtime";

function Card(props: ParentProps) {
  return <div class="card">{props.children}</div>;
}

export function Children() {
  return (
    <div class="app">
      <Card>
        <text class="title">Inside the card</text>
        <text>slotted via props.children</text>
      </Card>
    </div>
  );
}
