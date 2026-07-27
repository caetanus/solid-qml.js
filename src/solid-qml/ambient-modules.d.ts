// Ambient module declarations. This file is a SCRIPT (no top-level import/export) so the
// declarations register globally; `Component` is referenced via an inline import type.

// The `qml-solid` runtime API surface (Vite aliases it to ./runtime; the transpiler rewrites these
// to loader context properties on the native target).
declare module "qml-solid" {
  export const tabstop: { enabled: boolean };
  interface NotificationSignal {
    connect(handler: (...args: any[]) => void): void;
    disconnect(handler: (...args: any[]) => void): void;
  }
  export const notifications: {
    available: boolean;
    supportsActions: boolean;
    supportsReply: boolean;
    send(spec: Record<string, unknown>): number;
    close(id: number): void;
    actionInvoked: NotificationSignal;
    replied: NotificationSignal;
    closed: NotificationSignal;
  };
}

// Hand-written .qml files imported straight into TSX (`import Badge from "./Badge.qml"`).
declare module "*.qml" {
  const component: import("solid-js").Component<Record<string, unknown>>;
  export default component;
}
