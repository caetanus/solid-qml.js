// Dashboard — deliberate STRESS TEST for the CSS engine and the QML translation:
// sidebar menu + sub-pages (<Switch>), image carousel (timer + manual controls + dots),
// and <For> with COMPLEX items everywhere (nested sub-JSX, <Show> inside the delegate,
// classList driven by item fields, click handlers closing over the item).
import { createSignal, onCleanup, For, Index, Switch, Match, Show } from "solid-js";
import { div, text, button } from "../src/solid-qml/runtime";
import "./dashboard.css";

const MENU = [
  { id: "overview", label: "Overview", icon: "▤", badge: "" },
  { id: "analytics", label: "Analytics", icon: "◔", badge: "3" },
  { id: "projects", label: "Projects", icon: "▦", badge: "12" },
  { id: "activity", label: "Activity", icon: "≡", badge: "24" },
  { id: "settings", label: "Settings", icon: "✦", badge: "" },
];

const STATS = [
  { label: "Revenue", value: "48.2k", delta: "+12.4%", up: true },
  { label: "Active users", value: "1 284", delta: "+3.1%", up: true },
  { label: "Churn", value: "2.4%", delta: "-0.8%", up: false },
  { label: "Uptime", value: "99.98%", delta: "+0.01%", up: true },
];

// Image sources: "assets/…" works on BOTH targets — vite serves it via public/assets,
const SLIDES = [
  { src: "assets/photo-mountain.jpg", caption: "Ridge — object-fit: cover on a real photo" },
  { src: "assets/photo-forest.jpg", caption: "Riverbend — native Image decode" },
  { src: "assets/photo-city.jpg", caption: "Fog city — carousel crossfade" },
];

const FEED = [
  { who: "AC", name: "Ana Costa", what: "deployed dashboard v2 to production", when: "2 min", tag: "deploy", ok: true },
  { who: "RM", name: "Rui Matos", what: "opened PR #142 — carousel easing", when: "18 min", tag: "review", ok: true },
  { who: "LS", name: "Lia Souza", what: "pipeline failed on test-css step", when: "41 min", tag: "ci", ok: false },
  { who: "JP", name: "João Prado", what: "published qml-css-engine 0.2.0", when: "1 h", tag: "release", ok: true },
];

const PROJECTS = [
  { name: "qml-css-engine", desc: "CSS cascade, paint & layout in C++", pct: "p80", status: "active" },
  { name: "transpiler v2", desc: "Solid JSX → structural QML", pct: "p65", status: "active" },
  { name: "npm mirror", desc: "node modules on the V4 engine", pct: "p45", status: "beta" },
  { name: "AOT C++ target", desc: "QtQuick C++ 1:1 generation", pct: "p10", status: "design" },
];

// Runtime theme override — loaded via cssTheme.loadLayeredString ON TOP of the cascade (the
// live-CSS selling point: same rules engine, swapped at runtime). Cleared on unmount.
const DARK_CSS = `
.dash { background: #10161f; }
.dash-top { background: #16212e; border-bottom: 1px solid #223140; }
.dash-title { color: #e8eef5; }
.dash-user-n { color: #b9c7d4; }
.dash-user:hover { background: #1d2a38; }
.dash-user.open { background: #223140; }
.stat { background: linear-gradient(180deg, #16212e 0%, #131c27 100%); border: 1px solid #223140; border-top: 3px solid #41cd52; }
.stat:hover { border: 1px solid #2e4256; border-top: 3px solid #4fe063; }
.stat-label { color: #7d92a5; }
.stat-value { color: #e8eef5; }
.feed { background: #16212e; border: 1px solid #223140; }
.feed-h { color: #e8eef5; }
.feed-row:hover { background: #1d2a38; }
.feed-avatar { background: #223140; }
.feed-avatar-t { color: #b9c7d4; }
.feed-name { color: #e8eef5; }
.feed-what { color: #7d92a5; }
.feed-tag { background: #223140; }
.feed-tag-t { color: #b9c7d4; }
.feed-when { color: #4d6274; }
.dash .panel { background: #16212e; border: 1px solid #223140; }
.panel-h { color: #e8eef5; }
.prog-name { color: #b9c7d4; }
.prog-track { background: #223140; }
.axis { color: #4d6274; }
.proj-card { background: #16212e; border: 1px solid #223140; }
.proj-card:hover { border: 1px solid #2e4256; }
.proj-name { color: #e8eef5; }
.proj-desc { color: #7d92a5; }
.proj-hint { color: #4d6274; }
.set-row { border-bottom: 1px solid #223140; }
.set-label { color: #b9c7d4; }
.set-input { background: #131c27; border: 1px solid #223140; color: #e8eef5; }
.set-opt { background: #16212e; border: 1px solid #223140; }
.set-opt text { color: #b9c7d4; }
.set-opt:hover { background: #1d2a38; }
.user-card { background: #16212e; border: 1px solid #2e4256; }
.user-card-name { color: #e8eef5; }
.user-kv-v { color: #e8eef5; }
.user-card-row { border-bottom: 1px solid #223140; }
`;

// Second runtime override: pushes SHAPE-demanding styles onto elements that normally take
// the cheap Rectangle path — a live proof that the Shape x Rectangle policy swaps per apply.
const FANCY_CSS = `
.dash-badge { background: linear-gradient(135deg, #41cd52, #176b87); }
.stat-delta { background: linear-gradient(90deg, rgba(65,205,82,0.30), rgba(23,107,135,0.18)); }
.feed-tag { background: linear-gradient(90deg, #eaf1f8, #d7e6f2); }
.user-tag { background: linear-gradient(90deg, rgba(65,205,82,0.30), rgba(65,205,82,0.10)); }
.proj-pill { background: linear-gradient(90deg, rgba(65,205,82,0.30), rgba(65,205,82,0.10)); }
.prog-track { box-shadow: inset 0 1px 3px rgba(23,34,44,0.25); }
.set-opt { border-left: 3px solid #41cd52; }
.feed-avatar { background: linear-gradient(135deg, #e5ecf4, #cddcEB); }
.dash-item.active { background: linear-gradient(90deg, #176b87, #1d7fa0); }
`;

const LOG = [
  { who: "AC", what: "deployed dashboard v2 to production", tag: "deploy", ok: true },
  { who: "RM", what: "opened PR #142 — carousel easing curves", tag: "review", ok: true },
  { who: "LS", what: "pipeline failed on test-css step", tag: "ci", ok: false },
  { who: "JP", what: "published qml-css-engine 0.2.0", tag: "release", ok: true },
  { who: "MB", what: "rewrote the flex shrink pass in C++", tag: "engine", ok: true },
  { who: "AC", what: "fixed ancestor-scoped hover rules", tag: "engine", ok: true },
  { who: "TS", what: "added Flickable-backed overflow scroll", tag: "engine", ok: true },
  { who: "RM", what: "benchmarked 1100 nodes/page after lazy effects", tag: "perf", ok: true },
  { who: "LS", what: "nightly run red: fetch shim on HTTP/2", tag: "ci", ok: false },
  { who: "JP", what: "mirrored js-base64 onto the V4 engine", tag: "npm", ok: true },
  { who: "MB", what: "landed grid-template-areas mapping", tag: "engine", ok: true },
  { who: "AC", what: "shipped the collapsible sidebar", tag: "ui", ok: true },
  { who: "TS", what: "traced the RHI layer runaway to stale builds", tag: "perf", ok: true },
  { who: "RM", what: "reviewed the transpiler precedence fix", tag: "review", ok: true },
  { who: "LS", what: "flaky: keyframes driver timing on CI", tag: "ci", ok: false },
  { who: "JP", what: "documented QMLCss.h one-call registration", tag: "docs", ok: true },
  { who: "MB", what: "ported Contrast.js to a C++ singleton", tag: "engine", ok: true },
  { who: "AC", what: "wired hover tracking for any :hover rule", tag: "engine", ok: true },
  { who: "TS", what: "added text background Shape underlay", tag: "engine", ok: true },
  { who: "RM", what: "merged display:none binding preservation", tag: "review", ok: true },
  { who: "LS", what: "green across 43 engine tests", tag: "ci", ok: true },
  { who: "JP", what: "released the dashboard stress page", tag: "release", ok: true },
  { who: "MB", what: "profiled badge-pulse frame production", tag: "perf", ok: true },
  { who: "AC", what: "tuned the sidebar width transition", tag: "ui", ok: true },
];

export function Dashboard() {
  const [page, setPage] = createSignal("overview");
  const [slide, setSlide] = createSignal(0);
  const [collapsed, setCollapsed] = createSignal(false);
  const [userOpen, setUserOpen] = createSignal(false);
  const [order, setOrder] = createSignal(["qml-css-engine", "transpiler v2", "npm mirror", "AOT C++ target"]);
  const [darkTheme, setDarkTheme] = createSignal(false);
  const [fancy, setFancy] = createSignal(false);
  function applyOverrides() {
    cssTheme.loadLayeredString((darkTheme() ? DARK_CSS : "") + (fancy() ? FANCY_CSS : ""));
  }
  function toggleTheme() {
    setDarkTheme(!darkTheme());
    applyOverrides();
  }
  function toggleFancy() {
    setFancy(!fancy());
    applyOverrides();
  }
  onCleanup(() => cssTheme.loadLayeredString(""));
  function proj(n) {
    const hit = PROJECTS.filter((x) => x.name === n);
    return hit.length > 0 ? hit[0] : PROJECTS[0];
  }
  function reorder(list, src, dst) {
    if (src === dst) return list;
    const out = list.filter((x) => x !== src);
    out.splice(out.indexOf(dst), 0, src);
    return out;
  }
  const timer = setInterval(() => setSlide((slide() + 1) % 3), 4000);
  onCleanup(() => clearInterval(timer));

  return (
    <div class="dash">
      <div class="dash-side" classList={{ collapsed: collapsed() }}>
        <div class="dash-brand">
          <div class="dash-logo"><text class="dash-logo-q">Q</text></div>
          <text class="dash-name">solid-qml</text>
        </div>
        <For each={MENU}>
          {(m) => (
            <div class="dash-item" classList={{ active: page() === m.id }} onClick={() => setPage(m.id)}>
              <text class="dash-icon">{m.icon}</text>
              <text class="dash-label">{m.label}</text>
              <Show when={m.badge !== ""}>
                <div class="dash-badge"><text class="dash-badge-n">{m.badge}</text></div>
              </Show>
            </div>
          )}
        </For>
        <div class="dash-spring" />
        <div class="side-toggle" onClick={() => setCollapsed(!collapsed())}>
          <text class="side-toggle-t">{collapsed() ? "»" : "«"}</text>
        </div>
        <div class="dash-foot"><text class="dash-foot-t">engine stress test</text></div>
      </div>

      <div class="dash-main">
        <div class="dash-top">
          <div class="dash-title-wrap">
            <text class="dash-title">{page()}</text>
            <div class="dash-title-accent" />
          </div>
          <div class="theme-toggle" classList={{ on: darkTheme() }} onClick={() => toggleTheme()}>
            <text class="theme-toggle-t">{darkTheme() ? "☀" : "☾"}</text>
          </div>
          <div class="theme-toggle fancy" classList={{ on: fancy() }} onClick={() => toggleFancy()}>
            <text class="theme-toggle-t">✨</text>
          </div>
          <div class="dash-user" classList={{ open: userOpen() }} onClick={() => setUserOpen(!userOpen())}>
            {/* NOTE: an inline <>fragment</> here showcased the native splice, but
                dom-expressions (the WEB target) only accepts top-level fragments — and an
                inline fragment splices children exactly like having none, so both targets
                agree without it. Fragment coverage lives in the transpiler tests. */}
            <div class="dash-avatar-ring">
              <div class="dash-avatar"><text class="dash-avatar-t">AL</text></div>
            </div>
            <text class="dash-user-n">ada</text>
            <text class="dash-user-chev">▾</text>
            <Show when={userOpen()}>
              <div class="user-card">
                <div class="user-card-head">
                  <div class="user-card-avatar"><text class="user-card-avatar-t">AL</text></div>
                  <div class="user-card-id">
                    <text class="user-card-name">Ada Lovelace</text>
                    <text class="user-card-mail">ada@example.com</text>
                  </div>
                </div>
                <div class="user-card-tags">
                  <div class="user-tag"><text class="user-tag-t">owner</text></div>
                  <div class="user-tag alt"><text class="user-tag-t">engine dev</text></div>
                </div>
                <div class="user-card-row"><text class="user-kv">Plan</text><text class="user-kv-v">Max 20×</text></div>
                <div class="user-card-row"><text class="user-kv">Session</text><text class="user-kv-v">native · GPU</text></div>
                <div class="user-card-actions">
                  <button class="user-btn">Profile</button>
                  <button class="user-btn ghost">Sign out</button>
                </div>
              </div>
            </Show>
          </div>
        </div>

        <Switch>
          <Match when={page() === "overview"}>
            <div class="dash-page">
              <div class="stat-row">
                <For each={STATS}>
                  {(s) => (
                    <div class="stat">
                      <text class="stat-label">{s.label}</text>
                      <text class="stat-value">{s.value}</text>
                      <div class="stat-delta" classList={{ down: !s.up }}>
                        <text class="stat-delta-t">{s.delta}</text>
                      </div>
                    </div>
                  )}
                </For>
              </div>

              <div class="carousel">
                <Index each={SLIDES}>
                  {(s, i) => (
                    <div class="slide" classList={{ current: slide() === i }}>
                      <img class="slide-img" src={s().src} />
                      <div class="slide-cap"><text class="slide-cap-t">{s().caption}</text></div>
                    </div>
                  )}
                </Index>
                <button class="c-nav c-prev" onClick={() => setSlide((slide() + 2) % 3)}>‹</button>
                <button class="c-nav c-next" onClick={() => setSlide((slide() + 1) % 3)}>›</button>
                <div class="dots">
                  <Index each={SLIDES}>
                    {(s, i) => <div class="dot" classList={{ on: slide() === i }} onClick={() => setSlide(i)} />}
                  </Index>
                </div>
              </div>

              <div class="feed">
                <text class="feed-h">Recent activity</text>
                <For each={FEED}>
                  {(f) => (
                    <div class="feed-row">
                      <div class="feed-avatar"><text class="feed-avatar-t">{f.who}</text></div>
                      <div class="feed-body">
                        <text class="feed-name">{f.name}</text>
                        <text class="feed-what">{f.what}</text>
                      </div>
                      <div class="feed-tag"><text class="feed-tag-t">{f.tag}</text></div>
                      <Show when={!f.ok}><div class="feed-alert"><text class="feed-alert-t">!</text></div></Show>
                      <text class="feed-when">{f.when}</text>
                    </div>
                  )}
                </For>
              </div>
            </div>
          </Match>

          <Match when={page() === "analytics"}>
            <div class="dash-page">
              <div class="panel">
                <text class="panel-h">Weekly renders</text>
                <div class="chart">
                  <div class="bar b0" /><div class="bar b1" /><div class="bar b2" />
                  <div class="bar b3" /><div class="bar b4" /><div class="bar b5" />
                  <div class="bar b6" />
                </div>
                <div class="chart-x">
                  <text class="axis">mon</text><text class="axis">tue</text><text class="axis">wed</text>
                  <text class="axis">thu</text><text class="axis">fri</text><text class="axis">sat</text>
                  <text class="axis">sun</text>
                </div>
              </div>
              <div class="panel">
                <text class="panel-h">Pipeline health</text>
                <For each={PROJECTS}>
                  {(p) => (
                    <div class="prog-row">
                      <text class="prog-name">{p.name}</text>
                      <div class="prog-track"><div class={"prog-fill"} classList={{ p80: p.pct === "p80", p65: p.pct === "p65", p45: p.pct === "p45", p10: p.pct === "p10" }} /></div>
                    </div>
                  )}
                </For>
              </div>
            </div>
          </Match>

          <Match when={page() === "projects"}>
            <div class="dash-page">
              <text class="proj-hint">Drag a card onto another to reorder</text>
              <div class="proj-grid">
                <For each={order()}>
                  {(n) => (
                    <div class="proj-card" draggable dragData={n} onDrop={(src) => setOrder(reorder(order(), src, n))}>
                      <div class="proj-head">
                        <text class="proj-name">{proj(n).name}</text>
                        <div class="proj-pill" classList={{ beta: proj(n).status === "beta", design: proj(n).status === "design" }}>
                          <text class="proj-pill-t">{proj(n).status}</text>
                        </div>
                      </div>
                      <text class="proj-desc">{proj(n).desc}</text>
                      <div class="prog-track"><div class="prog-fill" classList={{ p80: proj(n).pct === "p80", p65: proj(n).pct === "p65", p45: proj(n).pct === "p45", p10: proj(n).pct === "p10" }} /></div>
                    </div>
                  )}
                </For>
              </div>
            </div>
          </Match>

          <Match when={page() === "activity"}>
            <div class="dash-page">
              <div class="feed tall">
                <text class="feed-h">Full activity log — scrolls natively</text>
                <For each={LOG}>
                  {(f) => (
                    <div class="feed-row">
                      <div class="feed-avatar"><text class="feed-avatar-t">{f.who}</text></div>
                      <div class="feed-body">
                        <text class="feed-what">{f.what}</text>
                      </div>
                      <div class="feed-tag"><text class="feed-tag-t">{f.tag}</text></div>
                      <Show when={!f.ok}><div class="feed-alert"><text class="feed-alert-t">!</text></div></Show>
                    </div>
                  )}
                </For>
              </div>
            </div>
          </Match>

          <Match when={page() === "settings"}>
            <div class="dash-page">
              <div class="panel">
                <text class="panel-h">Workspace</text>
                <div class="set-row">
                  <text class="set-label">Display name</text>
                  <input class="set-input" placeholder="ada" />
                </div>
                <div class="set-row">
                  <text class="set-label">Theme</text>
                  <div class="set-opts">
                    <button class="set-opt active">dark</button>
                    <button class="set-opt">light</button>
                    <button class="set-opt">system</button>
                  </div>
                </div>
                <div class="set-row">
                  <text class="set-label">GPU vsync</text>
                  <button class="set-opt active">on</button>
                </div>
              </div>
            </div>
          </Match>
        </Switch>
      </div>
    </div>
  );
}
