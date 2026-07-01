const vscode = require("vscode");
const path = require("path");
const fs = require("fs");

// Resolve qml-debug's standalone adapter: an explicit config path wins, else the installed
// `orcun-gokbulut.qml-debug` extension's bundled adapter.
function resolveInnerAdapter(config) {
  if (config.innerAdapter && fs.existsSync(config.innerAdapter)) return config.innerAdapter;
  const ext = vscode.extensions.getExtension("orcun-gokbulut.qml-debug");
  if (ext) {
    const p = path.join(ext.extensionPath, "out", "debug-adapter.js");
    if (fs.existsSync(p)) return p;
  }
  return undefined;
}

function activate(context) {
  const factory = {
    createDebugAdapterDescriptor(session) {
      const inner = resolveInnerAdapter(session.configuration);
      if (!inner) {
        vscode.window.showErrorMessage(
          "Solid-QML debug: qml-debug adapter not found. Install the 'qml-debug' extension " +
          "(orcun-gokbulut.qml-debug) or set 'innerAdapter' in launch.json."
        );
        return undefined;
      }
      const proxy = path.join(context.extensionPath, "proxy.mjs");
      // VS Code <-> this proxy (node) <-> qml-debug's adapter <-> Qt V4 engine.
      return new vscode.DebugAdapterExecutable("node", [proxy, "--inner", inner]);
    },
  };
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory("solid-qml", factory)
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
