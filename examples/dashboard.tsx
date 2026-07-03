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
  { id: "settings", label: "Settings", icon: "✦", badge: "" },
];

const STATS = [
  { label: "Revenue", value: "48.2k", delta: "+12.4%", up: true },
  { label: "Active users", value: "1 284", delta: "+3.1%", up: true },
  { label: "Churn", value: "2.4%", delta: "-0.8%", up: false },
  { label: "Uptime", value: "99.98%", delta: "+0.01%", up: true },
];

// Image sources resolve relative to the generated .qml (qml/solidqml/), hence ../../assets.
const SLIDES = [
  { src: "../../assets/slide-aurora.png", caption: "Aurora — GPU scene graph" },
  { src: "../../assets/slide-ocean.png", caption: "Ocean — CSS gradient engine" },
  { src: "../../assets/slide-ember.png", caption: "Ember — @keyframes driver" },
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

export function Dashboard() {
  const [page, setPage] = createSignal("overview");
  const [slide, setSlide] = createSignal(0);
  const [collapsed, setCollapsed] = createSignal(false);
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
          <div class="dash-user">
            <div class="dash-avatar-ring">
              <div class="dash-avatar"><text class="dash-avatar-t">AL</text></div>
            </div>
            <text class="dash-user-n">ada</text>
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
              <div class="proj-grid">
                <For each={PROJECTS}>
                  {(p) => (
                    <div class="proj-card">
                      <div class="proj-head">
                        <text class="proj-name">{p.name}</text>
                        <div class="proj-pill" classList={{ beta: p.status === "beta", design: p.status === "design" }}>
                          <text class="proj-pill-t">{p.status}</text>
                        </div>
                      </div>
                      <text class="proj-desc">{p.desc}</text>
                      <div class="prog-track"><div class="prog-fill" classList={{ p80: p.pct === "p80", p65: p.pct === "p65", p45: p.pct === "p45", p10: p.pct === "p10" }} /></div>
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
