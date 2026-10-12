import type { Plugin } from "vite";

export interface SolidQmlViteOptions {
  /** Module the legacy "qml-solid" alias and .qml placeholders resolve to. Default "solid-qml". */
  runtimeModule?: string;
}

/** Web preview compatibility for QML/Qt signal-style handlers and the <text> primitive. */
export function transformWebEvents(code: string): string;

export default function solidQml(options?: SolidQmlViteOptions): Plugin;
export { solidQml };
