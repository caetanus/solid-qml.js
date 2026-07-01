import { Box as RedBox } from "./red/Box";
import { Box as BlueBox } from "./blue/Box";

export function App() {
  return (
    <div class="app">
      <RedBox label="r" />
      <BlueBox label="b" />
    </div>
  );
}
