// Menus & views group (native-only plan, phase 3): <Menu>, <MenuBar>, <TreeView>, <Tray>.
// Native-only tags are registry-dispatched by the transpiler (no runtime import — the QML
// emitter resolves <Menu>/<MenuBar>/<TreeView>/<Tray> before user components); this module only
// ever renders in the QML loader (native.tsx gates it behind the process.versions.solidQml probe).
import { createSignal } from "solid-js";
import { div, text, button } from "../../src/solid-qml/runtime";
import "./menus.css";

declare const Menu: any, MenuItem: any, MenuSeparator: any, MenuBar: any, TreeView: any, ListView: any, TableView: any, Tray: any;

export function MenusAndViews() {
  const [lastAction, setLastAction] = createSignal("none yet");
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

      {/* ── menu bar ─────────────────────────────────────────────────────────── */}
      <div class="nv-row">
        <text class="nv-label">menubar</text>
        <MenuBar class="nv-menubar">
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

      {/* <Tray> is real (Platform.SystemTrayIcon) and TESTED, but NOT instantiated here:
          a SystemTrayIcon registers a StatusNotifierItem in the OS tray, and a gallery that
          is launched/killed constantly during dev would leave ghost icons behind (SIGKILL
          never deregisters the D-Bus item). It belongs in a real app with a clean shutdown,
          not a showcase. See transpiler/test/native-menus.test.ts for its emission. */}
      <div class="nv-row">
        <text class="nv-label">Tray</text>
        <text class="nv-echo">&lt;Tray&gt; — native SystemTrayIcon (not shown here; see tests)</text>
      </div>
    </div>
  );
}
