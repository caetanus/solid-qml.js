import { defineConfig } from "vite";
import solid from "vite-plugin-solid";
import solidQml from "solid-qml/vite";

// `npm run dev` is the browser preview. The native window is `npx solid-qml dev`; packaging is
// `npx solid-qml build --target linux`. Both transpile the SAME src/main.tsx to QML.
export default defineConfig({
  plugins: [solidQml(), solid()],
});
