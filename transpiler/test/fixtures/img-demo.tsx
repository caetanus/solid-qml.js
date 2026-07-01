import { createSignal } from "solid-js";

export function ImgDemo() {
  const [url, setUrl] = createSignal("qrc:/images/logo.png");
  return (
    <div class="container">
      <img class="hero" src={url()} />
    </div>
  );
}
