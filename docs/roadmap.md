# Roadmap & Gaps

## Roadmap

- **A single codebase for every platform** — Android, iOS, Windows, macOS and Linux (mobile
  **and** desktop) from the same Solid code.
- **AOT → C++ for release** — the generator emits 1:1 QtQuick C++, eliminating interpreted V4;
  Qt's V4-AOT covers only the residual dynamic JS (async/fetch/closures).
- **Real native C++ interop** alongside Solid components.
- **Importing real QML components** (interop / legacy code).
- **Decent packaging** for deployment, and a path to **exporting into legacy projects**.

## Gaps / Current limitations

Alpha — being honest about what **does not exist yet**:

- **Missing desktop integration**: D-Bus, Avahi/zeroconf and system tray, not yet.
- **Threads** and **background services (mobile)**, not yet.
- **Gestures** (touch), not yet.
- Overflow **scrolling**, not yet — QtQuick offers it via `Flickable`; it is at the top of the
  queue.
- `<For>` is still fragile with **nested sub-JSX** and complex cases.
- **Advanced CSS** mappings depend on open design decisions.
- **AOT → C++ not implemented yet** — today dev runs on interpreted V4.
- **Packaging with Qt is painful** — the deployment story is still open.
