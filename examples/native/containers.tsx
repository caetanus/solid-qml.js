// Native containers showcase (containers group of the native-only widgets plan):
// ToolBar, TabBar + StackView (tab switching), SplitView, Drawer, SwipeView + PageIndicator.
// The container tags are QML-only surface — no runtime import needed; the browser build never
// renders this subtree (guarded by the probe in examples/native.tsx).
import { createSignal } from "solid-js";
import { div, text, button } from "../../src/solid-qml/runtime";
import "./containers.css";

export function Containers() {
  const [tab, setTab] = createSignal(0);
  const [drawerOpen, setDrawerOpen] = createSignal(false);
  const [page, setPage] = createSignal(0);

  return (
    <div class="nv-section containers">
      <text class="nv-title">Containers</text>

      {/* ── ToolBar ─────────────────────────────────────────────── */}
      <text class="nv-label">ToolBar</text>
      <ToolBar class="ct-toolbar">
        <button class="ct-tool" onClick={() => setDrawerOpen(true)}>☰ Menu</button>
        <button class="ct-tool" onClick={() => setTab(0)}>Home</button>
        <button class="ct-tool" onClick={() => setPage(0)}>Reset pages</button>
      </ToolBar>

      {/* ── TabBar + StackView (controlled together) ───────────── */}
      <text class="nv-label">TabBar + StackView</text>
      <TabBar class="ct-tabs" current={tab()} onChange={(i) => setTab(i)}>
        <TabButton>Overview</TabButton>
        <TabButton>Details</TabButton>
        <TabButton>Settings</TabButton>
      </TabBar>
      <StackView class="ct-stack" current={tab()}>
        <div class="ct-page">
          <text class="ct-page-t">Overview — the first tab's page.</text>
        </div>
        <div class="ct-page">
          <text class="ct-page-t">Details — swap tabs above to switch this content.</text>
        </div>
        <div class="ct-page">
          <text class="ct-page-t">Settings — page three of the stack.</text>
        </div>
      </StackView>

      {/* ── SplitView ──────────────────────────────────────────── */}
      <text class="nv-label">SplitView</text>
      <SplitView class="ct-split" orientation="horizontal">
        <div class="ct-pane">
          <text class="ct-pane-t">Left pane — drag the handle.</text>
        </div>
        <div class="ct-pane ct-pane-alt">
          <text class="ct-pane-t">Right pane.</text>
        </div>
      </SplitView>

      {/* ── Drawer ─────────────────────────────────────────────── */}
      <text class="nv-label">Drawer</text>
      <div class="nv-row">
        <button class="ct-open" onClick={() => setDrawerOpen(true)}>Open drawer</button>
        <text class="ct-state">{drawerOpen() ? "open" : "closed"}</text>
      </div>
      <Drawer class="ct-drawer" open={drawerOpen()} edge="left" onClose={() => setDrawerOpen(false)}>
        <text class="ct-drawer-title">Navigation</text>
        <text class="ct-drawer-item">Home</text>
        <text class="ct-drawer-item">Profile</text>
        <text class="ct-drawer-item">Preferences</text>
        <button class="ct-close" onClick={() => setDrawerOpen(false)}>Close</button>
      </Drawer>

      {/* ── SwipeView + PageIndicator ──────────────────────────── */}
      <text class="nv-label">SwipeView</text>
      <SwipeView class="ct-swipe" current={page()} onChange={(i) => setPage(i)}>
        <div class="ct-slide ct-slide-a">
          <text class="ct-slide-t">Page one — swipe or use the buttons.</text>
        </div>
        <div class="ct-slide ct-slide-b">
          <text class="ct-slide-t">Page two.</text>
        </div>
        <div class="ct-slide ct-slide-c">
          <text class="ct-slide-t">Page three.</text>
        </div>
      </SwipeView>
      <div class="nv-row">
        <button class="ct-nav" onClick={() => setPage((page() + 2) % 3)}>‹ Prev</button>
        <PageIndicator class="ct-dots" count={3} current={page()} />
        <button class="ct-nav" onClick={() => setPage((page() + 1) % 3)}>Next ›</button>
      </div>
    </div>
  );
}
