// Menus & views group (native-only plan, phase 3): <Menu>, <MenuBar>, <TreeView>, <Tray>.
// Native-only tags are registry-dispatched by the transpiler (no runtime import — the QML
// emitter resolves <Menu>/<MenuBar>/<TreeView>/<Tray> before user components); this module only
// ever renders in the QML loader (native.tsx gates it behind the process.versions.solidQml probe).
import { createSignal, Show } from "solid-js";
import { div, text, button, notifications } from "../../src/solid-qml/runtime";
import "./menus.css";

export function MenusAndViews() {
  const [lastAction, setLastAction] = createSignal("none yet");
  const [trayOn, setTrayOn] = createSignal(true);
  const [trayEcho, setTrayEcho] = createSignal("tray idle");
  const [notifyEcho, setNotifyEcho] = createSignal(
    notifications.available ? "notification server ready" : "no notification server",
  );
  // Every sent message's body, echoed back with each event so the app SHOWS what happened
  // to which notification. Close reasons follow the Desktop Notifications spec.
  const [sentBodies] = createSignal<Record<number, string>>({});
  const msgOf = (id: number) => `#${id} \u201C${sentBodies()[id] || "?"}\u201D`;
  const reasonOf = (reason: number) => {
    if (reason === 1) return "expired";
    if (reason === 2) return "dismissed by the user";
    if (reason === 3) return "closed by the app";
    return `closed (reason ${reason})`;
  };
  const notifyPlain = () => {
    const body = "Hello from the native gallery";
    const id = notifications.send({ title: "solid-qml", body });
    sentBodies()[id] = body;
    setNotifyEcho(`sent ${msgOf(id)}`);
  };
  const notifyActions = () => {
    const body = "Pick an action — or reply right here";
    const id = notifications.send({
      title: "solid-qml",
      body,
      actions: [{ id: "ok", label: "OK" }, { id: "later", label: "Later" }],
      reply: "Type a reply…",
    });
    sentBodies()[id] = body;
    setNotifyEcho(`sent ${msgOf(id)} (actions${notifications.supportsReply ? " + reply" : ""})`);
  };
  notifications.actionInvoked.connect((id: number, action: string) =>
    setNotifyEcho(`${msgOf(id)} \u2192 action \u201C${action}\u201D`));
  notifications.replied.connect((id: number, replyText: string) =>
    setNotifyEcho(`${msgOf(id)} \u2192 reply: \u201C${replyText}\u201D`));
  notifications.closed.connect((id: number, reason: number) =>
    setNotifyEcho(`${msgOf(id)} \u2192 ${reasonOf(reason)}`));
  const [selNode, setSelNode] = createSignal("nothing");
  const [selItem, setSelItem] = createSignal("nothing");
  const [selRow, setSelRow] = createSignal("nothing");

  const treeData = [
    {
      label: "src",
      children: [
        { label: "emit", children: [{ label: "qml.ts" }, { label: "expr.ts" }] },
        { label: "resolve", children: [{ label: "node.ts" }] },
        { label: "index.ts" },
      ],
    },
    { label: "docs", children: [{ label: "roadmap.md" }] },
    { label: "package.json" },
  ];

  return (
    <div class="nv-section nv-menus">
      <text class="nv-title">Menus & views</text>

      {/* ── action menu: self-managed, toggled by its embedded trigger ──────── */}
      <div class="nv-row">
        <text class="nv-label">menu</text>
        <Menu trigger={<button>Actions ▾</button>}>
          <MenuItem onClick={() => setLastAction("new file")}>&New file</MenuItem>
          <MenuItem onClick={() => setLastAction("duplicate")}>&Duplicate</MenuItem>
          <MenuSeparator />
          <MenuItem onClick={() => setLastAction("delete")}>&Delete</MenuItem>
        </Menu>
        <text class="nv-echo">last action: {lastAction()}</text>
      </div>

      {/* ── menu bar (OS-native window chrome) ───────────────────────────────────
          <MenuBar> is a Qt.labs.platform MenuBar: it attaches to the window's title
          chrome (global menu on macOS / GNOME, in-window strip elsewhere), not the
          scene — so it takes no layout slot and carries no CSS. It renders nothing
          inline; look at the top of the window (or the OS global menu). */}
      <div class="nv-row">
        <text class="nv-label">menubar</text>
        <text class="nv-echo">(OS chrome — see the window title bar / global menu)</text>
        <MenuBar>
          <Menu title="&File">
            <MenuItem onClick={() => setLastAction("open…")}>&Open…</MenuItem>
            <MenuItem onClick={() => setLastAction("save")}>&Save</MenuItem>
            <MenuSeparator />
            <MenuItem onClick={() => setLastAction("quit")}>&Quit</MenuItem>
          </Menu>
          <Menu title="&Edit">
            <MenuItem onClick={() => setLastAction("copy")}>&Copy</MenuItem>
            <MenuItem onClick={() => setLastAction("paste")}>&Paste</MenuItem>
          </Menu>
        </MenuBar>
      </div>

      {/* ── recursive tree view ──────────────────────────────────────────────── */}
      <div class="nv-row nv-tree-row-host">
        <text class="nv-label">treeview</text>
        <TreeView class="nv-tree" data={treeData} onSelect={(node) => setSelNode(node.label)} />
        <text class="nv-echo">selected: {selNode()}</text>
      </div>

      {/* ── virtualized list view (real QtQuick ListView) ────────────────────── */}
      <div class="nv-row nv-tree-row-host">
        <text class="nv-label">listview</text>
        <ListView
          class="nv-list"
          data={["Inbox", "Starred", "Sent", "Drafts", "Spam", "Trash", "Archive"]}
          onSelect={(item) => setSelItem(item)}
        />
        <text class="nv-echo">selected: {selItem()}</text>
      </div>

      {/* ── table view (header + virtualized rows, dynamic columns) ───────────── */}
      <div class="nv-row nv-tree-row-host">
        <text class="nv-label">tableview</text>
        <TableView
          class="nv-table"
          columns={[{ key: "name", label: "Name" }, { key: "role", label: "Role" }, { key: "lang", label: "Lang" }]}
          data={[
            { name: "Ada Lovelace", role: "Analyst", lang: "Note G" },
            { name: "Alan Turing", role: "Logician", lang: "Machine" },
            { name: "Grace Hopper", role: "Compiler", lang: "COBOL" },
            { name: "Dennis Ritchie", role: "Systems", lang: "C" },
          ]}
          onSelect={(row) => setSelRow(row.name)}
        />
        <text class="nv-echo">selected: {selRow()}</text>
      </div>

      {/* ── ContextMenu: right-click anywhere on the zone ──────── */}
      <div class="nv-row">
        <text class="nv-label">context</text>
        <div class="nv-ctxzone">
          <ContextMenu>
            <MenuItem onClick={() => setLastAction("ctx: copy")}>&Copy</MenuItem>
            <MenuItem onClick={() => setLastAction("ctx: paste")}>&Paste</MenuItem>
            <MenuSeparator />
            <MenuItem onClick={() => setLastAction("ctx: delete")}>&Delete</MenuItem>
          </ContextMenu>
          <text class="nv-ctxzone-t">right-click here</text>
        </div>
        <text class="nv-echo">last action: {lastAction()}</text>
      </div>

      {/* ── Accelerators: app-wide keyboard shortcuts ──────────── */}
      <div class="nv-row">
        <text class="nv-label">accel</text>
        <Shortcut keys="Ctrl+S" onActivated={() => setLastAction("accel: Ctrl+S (save)")} />
        <Shortcut keys={["Ctrl+K", "Ctrl+Shift+P"]} onActivated={() => setLastAction("accel: Ctrl+K (palette)")} />
        <text class="nv-echo">press Ctrl+S or Ctrl+K anywhere \u2192 last action updates</text>
      </div>

      {/* ── Tray + desktop notifications ─────────────────────────────────────────
          The tray icon is REAL (Platform.SystemTrayIcon): its menu items call back into this
          component, and the toggle removes it cleanly (visible=false deregisters the D-Bus
          StatusNotifierItem — no ghost icons). Notifications ride the loader's
          org.freedesktop.Notifications shim (D-Bus worker thread); every event the server
          sends back — action clicks, inline replies, closes — echoes below. */}
      <div class="nv-row">
        <text class="nv-label">Tray</text>
        <Show when={trayOn()}>
          <Tray tooltip="solid-qml gallery" icon="assets/logo.png"
                onActivate={() => setTrayEcho("icon activated")}>
            <MenuItem onClick={() => setTrayEcho("tray menu: open")}>&Open gallery</MenuItem>
            <MenuItem onClick={() => notifyPlain()}>&Notify</MenuItem>
            <MenuSeparator />
            <MenuItem onClick={() => setTrayOn(false)}>&Remove icon</MenuItem>
          </Tray>
        </Show>
        <button class="hw-btn" onClick={() => setTrayOn(!trayOn())}>
          {trayOn() ? "remove tray icon" : "show tray icon"}
        </button>
        <text class="nv-echo">{trayEcho()}</text>
      </div>

      <div class="nv-row">
        <text class="nv-label">notify</text>
        <button class="hw-btn" onClick={() => notifyPlain()}>simple</button>
        <button class="hw-btn" onClick={() => notifyActions()}>actions + reply</button>
        <text class="nv-echo">{notifyEcho()}</text>
      </div>
    </div>
  );
}
