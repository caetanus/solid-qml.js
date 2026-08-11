// The C++ (AOT) back-end: a second emitter over the SAME normalized Babel AST that emit/qml.ts
// walks. Where the QML back-end prints a `.qml` string that V4 interprets at runtime, this prints
// C++ that constructs the identical widget tree directly — dropping the interpreter from the hot
// path (structure, bindings, handlers). Emission is a template pass, no new toolchain (per ROADMAP).
//
// Scope: M-AOT-0 — the counter construct set (component state, div/button/text elements, static
// props, string-concat bindings, setter handlers, nested component instances, a Window root). Any
// construct outside that set throws loudly (the M-AOT-1 gap queue), never emits silent wrong code.
//
// The generated shape is proven pixel-identical to the QML back-end by examples/aot/proof — this
// emitter reproduces that hand-written pattern from the AST.
import * as t from "@babel/types";
import { normalize } from "../../babel/transform.ts";
import { analyzeProps, analyzeSignals, type SymbolTable } from "../../model/symbols.ts";
import { hParts, isHCall } from "../../ast/h.ts";

export interface GeneratedCpp {
  /** State classes (Q_OBJECT) — needs moc. */
  header: string;
  /** Build functions. */
  source: string;
  /** app_main.cpp — emitted only when an entry component has a <Window> root. */
  main: string | null;
  /** The entry component's build function + window metadata (for the driver / build wiring). */
  entry: { buildFn: string; width: number; height: number; title: string };
}

const TEXT_TAGS = new Set(["text", "span", "h1", "h2", "h3", "h4", "h5", "h6", "p", "cite", "bio"]);

interface CompInfo {
  name: string;
  fn: t.Function;
  render: t.CallExpression;
}

/** Every top-level component function (returns an h() call). */
function findComponents(ast: t.File): Map<string, CompInfo> {
  const out = new Map<string, CompInfo>();
  const consider = (fn: t.Function | null | undefined, name: string | null) => {
    if (!fn || !name || !t.isBlockStatement(fn.body)) return;
    for (const s of fn.body.body)
      if (t.isReturnStatement(s) && isHCall(s.argument)) {
        out.set(name, { name, fn, render: s.argument });
        return;
      }
  };
  for (const node of ast.program.body) {
    if (t.isFunctionDeclaration(node) && node.id) consider(node, node.id.name);
    if (t.isExportNamedDeclaration(node) && node.declaration && t.isFunctionDeclaration(node.declaration) && node.declaration.id)
      consider(node.declaration, node.declaration.id.name);
    if (t.isExportDefaultDeclaration(node) && t.isFunctionDeclaration(node.declaration) && node.declaration.id)
      consider(node.declaration, node.declaration.id.name);
  }
  return out;
}

const cap = (s: string) => s.charAt(0).toUpperCase() + s.slice(1);
const cppStr = (s: string) => `QStringLiteral("${s.replace(/\\/g, "\\\\").replace(/"/g, '\\"').replace(/\n/g, "\\n")}")`;

/** Expression scope: which identifiers are the component's reactive cells / props / setters, plus the
 *  owning state class name (connect targets like `&CounterState::countChanged`). */
interface Scope {
  table: SymbolTable;
  propsParam: string | null;
  props: Set<string>;
  components: Set<string>;
  stateClass: string;
}

/** Is `name` a state-backed reactive/prop cell (→ `state->name()`)? */
function isStateRead(name: string, sc: Scope): boolean {
  const sym = sc.table.get(name);
  return sc.props.has(name) || (!!sym && (sym.kind === "signal" || sym.kind === "memo" || sym.kind === "store"));
}

/** Translate a JS expression to a C++ QVariant expression (the sq:: runtime carries JS coercions). */
function emitExpr(node: t.Node, sc: Scope): string {
  if (t.isParenthesizedExpression(node)) return emitExpr(node.expression, sc);
  if (t.isStringLiteral(node)) return `QVariant(${cppStr(node.value)})`;
  if (t.isNumericLiteral(node)) return `QVariant(${Number.isInteger(node.value) ? node.value : `${node.value}`})`;
  if (t.isBooleanLiteral(node)) return `QVariant(${node.value})`;
  if (t.isIdentifier(node)) {
    if (isStateRead(node.name, sc)) return `state->${node.name}()`;
    throw new Error(`AOT: unsupported identifier "${node.name}" (not a signal/prop of the component)`);
  }
  // `count()` — a signal/memo accessor call with no args → the state getter.
  if (t.isCallExpression(node) && t.isIdentifier(node.callee) && node.arguments.length === 0 && isStateRead(node.callee.name, sc))
    return `state->${node.callee.name}()`;
  // `props.label` → the state getter of the same name.
  if (t.isMemberExpression(node) && !node.computed && t.isIdentifier(node.object) && t.isIdentifier(node.property)
      && node.object.name === sc.propsParam)
    return `state->${node.property.name}()`;
  if (t.isBinaryExpression(node) && t.isExpression(node.left)) {
    const op = ({ "+": "add", "-": "sub", "*": "mul", "/": "div" } as Record<string, string>)[node.operator];
    if (!op) throw new Error(`AOT: unsupported binary operator "${node.operator}"`);
    return `sq::${op}(${emitExpr(node.left, sc)}, ${emitExpr(node.right, sc)})`;
  }
  throw new Error(`AOT: unsupported expression node "${node.type}"`);
}

/** Collect the reactive dependency identifiers (signals/props) an expression reads. */
function collectDeps(node: t.Node, sc: Scope, out: Set<string>): void {
  if (t.isIdentifier(node) && isStateRead(node.name, sc)) out.add(node.name);
  if (t.isCallExpression(node) && t.isIdentifier(node.callee) && node.arguments.length === 0 && isStateRead(node.callee.name, sc))
    out.add(node.callee.name);
  if (t.isMemberExpression(node) && !node.computed && t.isIdentifier(node.object) && t.isIdentifier(node.property)
      && node.object.name === sc.propsParam)
    out.add(node.property.name);
  for (const key of Object.keys(node)) {
    const v = (node as unknown as Record<string, unknown>)[key];
    if (Array.isArray(v)) for (const c of v) { if (c && typeof (c as t.Node).type === "string") collectDeps(c as t.Node, sc, out); }
    else if (v && typeof (v as t.Node).type === "string") collectDeps(v as t.Node, sc, out);
  }
}

interface ElemProps { classes: string[]; onClick: t.Node | undefined; }
function readProps(propsArg: t.Node | undefined): ElemProps {
  const out: ElemProps = { classes: [], onClick: undefined };
  if (!propsArg || !t.isObjectExpression(propsArg)) return out;
  for (const p of propsArg.properties) {
    if (!t.isObjectProperty(p) || !t.isIdentifier(p.key)) continue;
    if (p.key.name === "class" && t.isStringLiteral(p.value)) out.classes = p.value.value.split(/\s+/).filter(Boolean);
    else if (p.key.name === "onClick") out.onClick = p.value;
  }
  return out;
}

const isElement = (n: t.Node) => isHCall(n);

/** Build a `setText(...)` string-concat binding + its dependency connects for the non-element
 *  children of a button/text. Returns the C++ statements (into `out`) targeting `btnVar`. */
function emitTextBinding(children: t.Node[], targetVar: string, sc: Scope, out: string[]): void {
  const parts = children.filter((c) => !isElement(c)).map((c) => {
    if (t.isStringLiteral(c)) return c.value.trim() || /\s/.test(c.value) ? `QVariant(${cppStr(c.value)})` : null;
    if (t.isJSXText(c)) { const s = c.value.replace(/\s+/g, " "); return s.trim() ? `QVariant(${cppStr(s)})` : null; }
    if (t.isExpression(c)) return emitExpr(c, sc);
    return null;
  }).filter((s): s is string => s !== null);
  // Fold as JS `"" + p0 + p1 + ...` so the result is always a string.
  let expr = "QVariant(QString())";
  for (const p of parts) expr = `sq::add(${expr}, ${p})`;
  const setText = `${targetVar}->setText(sq::str(${expr}));`;
  const deps = new Set<string>();
  for (const c of children) if (!isElement(c) && typeof (c as t.Node).type === "string") collectDeps(c, sc, deps);
  if (deps.size === 0) {
    out.push(setText);
    return;
  }
  // A connect-driven update lambda over each dependency's NOTIFY signal.
  out.push(`{ auto __upd = [${targetVar}, state] { ${setText} };`);
  for (const d of deps) out.push(`  QObject::connect(state, &${sc.stateClass}::${d}Changed, ${targetVar}, __upd);`);
  out.push(`  __upd(); }`);
}

/** Emit the C++ that builds one element subtree into a fresh var; returns that var name. */
function emitElement(node: t.CallExpression, sc: Scope, ctx: string, out: string[], fresh: () => string): string {
  const { tag, props: propsArg, children } = hParts(node);

  // Nested component instance: `<Counter label="A" />`.
  if (t.isIdentifier(tag) && sc.components.has(tag.name)) {
    const v = fresh();
    const stateVar = `${v}_st`;
    out.push(`auto *${stateVar} = new ${tag.name}State();`);
    if (propsArg && t.isObjectExpression(propsArg)) {
      for (const p of propsArg.properties)
        if (t.isObjectProperty(p) && t.isIdentifier(p.key) && p.key.name !== "children" && t.isExpression(p.value))
          out.push(`${stateVar}->set${cap(p.key.name)}(${emitExpr(p.value, sc)});`);
    }
    out.push(`auto *${v} = build${tag.name}(ctx, ${stateVar});`, `${stateVar}->setParent(${v}); // lifetime tied to the built item`);
    return v;
  }
  if (!t.isStringLiteral(tag)) throw new Error(`AOT: unsupported tag ${tag.type}`);
  const tagName = tag.value;
  const p = readProps(propsArg);
  const v = fresh();

  if (tagName === "div") {
    // A <div> is a pure container in M-AOT-0; raw text content (which the web wraps in an anonymous
    // text node) is not modelled yet — fail loud rather than drop it silently.
    for (const c of children)
      if (!isElement(c) && ((t.isStringLiteral(c) && c.value.trim()) || (t.isJSXText(c) && c.value.trim()) || t.isExpression(c) && !t.isStringLiteral(c) && !t.isJSXText(c)))
        throw new Error("AOT: raw text content in <div> is not supported (wrap it in <text>)");
    out.push(`auto *${v} = new SolidWidgets::Div();`, `sq::begin(${v}, ctx);`);
    if (p.classes.length) out.push(`${v}->setCssClass(sq::classes({${p.classes.map((c) => `"${c}"`).join(", ")}}));`);
    for (const c of children) if (isElement(c)) { const cv = emitElement(c as t.CallExpression, sc, ctx, out, fresh); out.push(`sq::append(${v}, ${cv});`); }
    out.push(`sq::complete(${v});`);
    return v;
  }
  if (tagName === "button") {
    out.push(`auto *${v} = new SolidWidgets::Button();`, `sq::begin(${v}, ctx);`);
    if (p.classes.length) out.push(`${v}->setCssClass(sq::classes({${p.classes.map((c) => `"${c}"`).join(", ")}}));`);
    if (p.onClick) {
      if (!t.isArrowFunctionExpression(p.onClick) && !t.isFunctionExpression(p.onClick)) throw new Error("AOT: onClick must be an inline arrow");
      if (t.isBlockStatement(p.onClick.body)) throw new Error("AOT: block-bodied onClick not supported");
      out.push(`QObject::connect(${v}, &SolidWidgets::Button::clicked, state, [state] { ${emitHandler(p.onClick.body, sc)} });`);
    }
    // element children first (so the label composes after), then the text binding.
    for (const c of children) if (isElement(c)) { const cv = emitElement(c as t.CallExpression, sc, ctx, out, fresh); out.push(`sq::append(${v}, ${cv});`); }
    out.push(`sq::complete(${v});`);
    emitTextBinding(children, v, sc, out);
    return v;
  }
  if (TEXT_TAGS.has(tagName)) {
    out.push(`auto *${v} = new SolidWidgets::Text();`, `sq::begin(${v}, ctx);`);
    if (tagName !== "text") out.push(`${v}->setCssPrimitive(${cppStr(tagName)});`);
    out.push(`sq::complete(${v});`);
    emitTextBinding(children, v, sc, out);
    return v;
  }
  throw new Error(`AOT: unsupported element <${tagName}>`);
}

/** A setter-call handler body (`setCount(count() + 1)`) → `state->setCount(...);`. */
function emitHandler(body: t.Expression, sc: Scope): string {
  if (t.isCallExpression(body) && t.isIdentifier(body.callee)) {
    const sym = sc.table.get(body.callee.name);
    if (sym && sym.kind === "setter") {
      const arg = body.arguments[0];
      if (!arg || !t.isExpression(arg)) throw new Error("AOT: setter handler needs one expression arg");
      return `state->set${cap(sym.signal)}(${emitExpr(arg, sc)});`;
    }
  }
  throw new Error(`AOT: unsupported handler body "${body.type}" (only a signal setter call)`);
}

interface WindowMeta { width: number; height: number; title: string; child: t.CallExpression }
/** Read a <Window> root's props and its single element child. */
function readWindow(render: t.CallExpression): WindowMeta {
  const { props, children } = hParts(render);
  let width = 640, height = 480, title = "";
  if (props && t.isObjectExpression(props))
    for (const p of props.properties)
      if (t.isObjectProperty(p) && t.isIdentifier(p.key) && t.isExpression(p.value)) {
        if (p.key.name === "width" && t.isNumericLiteral(p.value)) width = p.value.value;
        if (p.key.name === "height" && t.isNumericLiteral(p.value)) height = p.value.value;
        if (p.key.name === "title" && t.isStringLiteral(p.value)) title = p.value.value;
      }
  const child = children.find(isElement);
  if (!child) throw new Error("AOT: <Window> needs one element child");
  return { width, height, title, child: child as t.CallExpression };
}

export async function generateCpp(source: string, filename: string): Promise<GeneratedCpp> {
  const { ast } = await normalize(source, filename);
  if (!ast) throw new Error("AOT: normalize produced no AST");
  const comps = findComponents(ast);
  if (comps.size === 0) throw new Error("AOT: no components found");
  const componentNames = new Set(comps.keys());

  // Entry = a component whose render root is <Window> (identifier "Window").
  let entryName: string | null = null;
  for (const [name, info] of comps) {
    const { tag } = hParts(info.render);
    if (t.isIdentifier(tag, { name: "Window" })) { entryName = name; break; }
  }
  if (!entryName) throw new Error("AOT: no <Window>-rooted entry component (M-AOT-0 needs an app window)");

  const headers: string[] = [];
  const sources: string[] = [];
  const forwards: string[] = [];

  for (const [name, info] of comps) {
    const table = analyzeSignals(info.fn);
    const propsInfo = analyzeProps(info.fn);
    const propNames = propsInfo.used.filter((n) => n !== "children");
    // The reactive cells that become QVariant state properties (signals/memos/stores), plus props.
    const cells = new Map<string, t.Expression | null>();
    for (const sym of table.values())
      if (sym.kind === "signal" || sym.kind === "store") cells.set(sym.name, sym.init ?? null);
    for (const n of propNames) if (!cells.has(n)) cells.set(n, null);

    const sc: Scope & { table: SymbolTable; propsParam: string | null; props: Set<string>; components: Set<string> } = {
      stateClass: `${name}State`,
      table,
      propsParam: propsInfo.param,
      props: new Set(propNames),
      components: componentNames,
    };

    const isEntry = name === entryName;
    const hasState = cells.size > 0;

    // ── State class ────────────────────────────────────────────────────────────────────────────
    if (hasState) {
      const props: string[] = [];
      const getset: string[] = [];
      const signals: string[] = [];
      const members: string[] = [];
      for (const [cell, init] of cells) {
        props.push(`    Q_PROPERTY(QVariant ${cell} READ ${cell} WRITE set${cap(cell)} NOTIFY ${cell}Changed)`);
        getset.push(
          `    QVariant ${cell}() const { return m_${cell}; }`,
          `    void set${cap(cell)}(const QVariant &v) { if (m_${cell} == v) return; m_${cell} = v; emit ${cell}Changed(); }`,
        );
        signals.push(`    void ${cell}Changed();`);
        const initExpr = init ? emitInitLiteral(init) : "";
        members.push(`    QVariant m_${cell}${initExpr ? ` = ${initExpr}` : ""};`);
      }
      headers.push(
        `class ${name}State : public QObject\n{\n    Q_OBJECT`,
        props.join("\n"),
        `public:\n    explicit ${name}State(QObject *parent = nullptr) : QObject(parent) {}`,
        getset.join("\n"),
        `signals:\n${signals.join("\n")}`,
        `private:\n${members.join("\n")}\n};\n`,
      );
    }

    // ── Build function ─────────────────────────────────────────────────────────────────────────
    const out: string[] = [];
    let n = 0;
    const fresh = () => `v${n++}`;
    let signature: string;
    if (isEntry) {
      // <Window> root → build the window-primitive box and the child subtree inside it.
      const win = readWindow(info.render);
      out.push(`auto *winBox = new QmlCss::CssRect();`, `sq::begin(winBox, ctx);`,
        `winBox->setCssPrimitive(QStringLiteral("window"));`, `winBox->setCssClass(sq::classes({"qml-window"}));`);
      const childVar = emitElement(win.child, sc, "ctx", out, fresh);
      out.push(`sq::append(winBox, ${childVar});`, `sq::complete(winBox);`, `return winBox;`);
      signature = `QQuickItem *build${name}(QQmlContext *ctx)`;
    } else {
      const childVar = emitElement(info.render, sc, "ctx", out, fresh);
      out.push(`return ${childVar};`);
      signature = hasState
        ? `QQuickItem *build${name}(QQmlContext *ctx, ${name}State *state)`
        : `QQuickItem *build${name}(QQmlContext *ctx)`;
    }
    forwards.push(`${signature};`);
    sources.push(`${signature}\n{\n    ${out.join("\n    ")}\n}\n`);
  }

  const entryInfo = comps.get(entryName)!;
  const win = readWindow(entryInfo.render);

  const header = [
    "// Generated by the AOT (C++) back-end. Do not edit by hand.",
    "#pragma once",
    "",
    "#include <QObject>",
    "#include <QVariant>",
    "#include <QtQml/qqml.h>",
    "",
    "class QQmlContext;",
    "class QQuickItem;",
    "",
    "namespace aot {",
    "",
    ...headers,
    `QQuickItem *build${entryName}(QQmlContext *ctx);`,
    "",
    "} // namespace aot",
    "",
  ].join("\n");

  const src = [
    "// Generated by the AOT (C++) back-end. Do not edit by hand.",
    '#include "generated.h"',
    "",
    '#include "aot/sqbuild.h"',
    '#include "aot/sqruntime.h"',
    "",
    '#include "qmlcss/cssrect.h"',
    '#include "widgets/button.h"',
    '#include "widgets/primitives.h"',
    "",
    "#include <QQmlContext>",
    "",
    "namespace aot {",
    "",
    "// Forward declarations (build order independent of component declaration order).",
    ...forwards,
    "",
    ...sources,
    "} // namespace aot",
    "",
  ].join("\n");

  const main = emitMain(entryName, win);
  return { header, source: src, main, entry: { buildFn: `build${entryName}`, width: win.width, height: win.height, title: win.title } };
}

/** A pure-literal initializer for a state member (`property var count: 0`). */
function emitInitLiteral(node: t.Expression): string {
  if (t.isNumericLiteral(node)) return `${Number.isInteger(node.value) ? node.value : node.value}`;
  if (t.isStringLiteral(node)) return cppStr(node.value);
  if (t.isBooleanLiteral(node)) return `${node.value}`;
  if (t.isUnaryExpression(node) && node.operator === "-" && t.isNumericLiteral(node.argument)) return `-${node.argument.value}`;
  return ""; // undefined default
}

function emitMain(entryName: string, win: WindowMeta): string {
  return [
    "// Generated by the AOT (C++) back-end. Do not edit by hand.",
    "// app_main: mirrors src/loader.cpp minus QML loading — wires engine/theme/layout, builds the",
    "// generated scene in C++, hosts it in a QQuickWindow, supports the offscreen --grab/--click.",
    '#include "generated.h"',
    "",
    '#include "qmlcss/QMLCss.h"',
    '#include "qmlcss/csslayout.h"',
    '#include "qmlcss/csstheme.h"',
    '#include "widgets/solidwidgets.h"',
    "",
    "#include <QApplication>",
    "#include <QCommandLineParser>",
    "#include <QMouseEvent>",
    "#include <QQmlContext>",
    "#include <QQmlEngine>",
    "#include <QQuickItem>",
    "#include <QQuickWindow>",
    "#include <QTimer>",
    "",
    "int main(int argc, char **argv)",
    "{",
    "    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);",
    "    QApplication app(argc, argv);",
    "    QmlCss::registerTypes();",
    "    SolidWidgets::registerTypes();",
    "",
    "    QCommandLineParser parser;",
    "    parser.addHelpOption();",
    '    parser.addOption({ QStringLiteral("css"), QStringLiteral("CSS file(s), layered."), QStringLiteral("path") });',
    `    parser.addOption({ QStringLiteral("width"), QStringLiteral("Window width."), QStringLiteral("px"), QStringLiteral("${win.width}") });`,
    `    parser.addOption({ QStringLiteral("height"), QStringLiteral("Window height."), QStringLiteral("px"), QStringLiteral("${win.height}") });`,
    '    parser.addOption({ QStringLiteral("grab"), QStringLiteral("Render one frame to PNG and exit."), QStringLiteral("png") });',
    '    parser.addOption({ QStringLiteral("click"), QStringLiteral("Synthesize a left click at \\"x,y\\" (repeatable)."), QStringLiteral("x,y") });',
    "    parser.process(app);",
    "",
    '    const int w = parser.value(QStringLiteral("width")).toInt();',
    '    const int h = parser.value(QStringLiteral("height")).toInt();',
    "",
    "    QQmlEngine engine;",
    "    QmlCss::CssTheme theme;",
    "    QmlCss::CssLayoutEngine layout(&theme);",
    "    QQmlContext *ctx = engine.rootContext();",
    '    ctx->setContextProperty(QStringLiteral("cssTheme"), &theme);',
    '    ctx->setContextProperty(QStringLiteral("cssLayout"), &layout);',
    '    theme.loadLayered(parser.values(QStringLiteral("css")));',
    "",
    "    QQuickWindow window;",
    "    window.setWidth(w);",
    "    window.setHeight(h);",
    `    window.setTitle(${cppStr(win.title)});`,
    "    theme.setViewport(w, h);",
    "    QObject::connect(&window, &QQuickWindow::widthChanged, &theme, [&] { theme.setViewport(window.width(), window.height()); });",
    "    QObject::connect(&window, &QQuickWindow::heightChanged, &theme, [&] { theme.setViewport(window.width(), window.height()); });",
    "",
    `    QQuickItem *root = aot::build${entryName}(ctx);`,
    "    root->setParentItem(window.contentItem());",
    "    root->setSize(QSizeF(w, h));",
    "    QObject::connect(&window, &QQuickWindow::widthChanged, root, [root, &window] { root->setWidth(window.width()); });",
    "    QObject::connect(&window, &QQuickWindow::heightChanged, root, [root, &window] { root->setHeight(window.height()); });",
    "    window.show();",
    "",
    '    const int grabMs = qEnvironmentVariableIntValue("SQ_GRAB_MS") > 0 ? qEnvironmentVariableIntValue("SQ_GRAB_MS") : 1400;',
    '    const QStringList clicks = parser.values(QStringLiteral("click"));',
    "    if (!clicks.isEmpty()) {",
    "        QTimer::singleShot(grabMs / 2, &window, [&window, clicks] {",
    "            for (const QString &c : clicks) {",
    '                const QStringList xy = c.split(QLatin1Char(\',\'));',
    "                if (xy.size() != 2) continue;",
    "                const QPointF p(xy[0].toDouble(), xy[1].toDouble());",
    "                const QPointF g = window.mapToGlobal(p);",
    "                QMouseEvent press(QEvent::MouseButtonPress, p, g, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);",
    "                QMouseEvent release(QEvent::MouseButtonRelease, p, g, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);",
    "                QCoreApplication::sendEvent(&window, &press);",
    "                QCoreApplication::sendEvent(&window, &release);",
    "            }",
    "        });",
    "    }",
    '    if (parser.isSet(QStringLiteral("grab"))) {',
    '        const QString grabPath = parser.value(QStringLiteral("grab"));',
    "        QTimer::singleShot(grabMs, &window, [&window, grabPath] {",
    "            const QImage frame = window.grabWindow();",
    "            if (frame.save(grabPath)) qInfo(\"aot: wrote %s\", qUtf8Printable(grabPath));",
    "            else qWarning(\"aot: failed to write %s\", qUtf8Printable(grabPath));",
    "            QCoreApplication::quit();",
    "        });",
    "    }",
    "    return app.exec();",
    "}",
    "",
  ].join("\n");
}
