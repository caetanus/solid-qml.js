// C23 (owner decision 2026-10-11) app-level contract test, run by the loader (tests/c23/run.sh).
// Two <For>s over the same kind of rows: DEFAULT (row identity = the element's reference, like Solid
// web) and KEYED (`key="id"`, opt-in reuse by field). Every row is a stateful component: a local
// signal plus an uncontrolled text field holding the same "typed text" (both must survive or reset
// together), onMount (registers the row instance under a fresh uid) and onCleanup (unregisters it). Each step edits the lists, waits a tick (a destroyed row's cleanup runs on its
// deferred deletion) and checks: which row INSTANCE shows each id, how many mounted/cleaned up, and
// whether row-local state survived. Prints `C23 PASS|FAIL <step> <detail>` and finally `C23 done`.
// (Native-only: the row's label probe reads the delegate's modelData — on the web it would read the
// live JS object, so the "mutate" step's not-reread check is about the native row's data; and the text
// field's content is its native `text`, the web's `value`.)
import { createSignal, For, onMount, onCleanup } from "solid-js";
import { div, text, input } from "../../src/solid-qml/runtime";

type Item = { id: number; label: string };

function IdRow(props: {
  row: Item;
  list: string;
  register: (list: string, id: number, set: (v: string) => void, get: () => string, label: () => string) => number;
  unregister: (uid: number) => void;
}) {
  const [local, setLocal] = createSignal("");
  let field;
  let uid = 0;
  const type = (v) => {
    setLocal(v);
    field.text = v;
    return 0;
  };
  // The typed text, when the signal and the field agree (both reset with a new row, both kept by a reused one).
  // (A block body: an expression-bodied arrow reading a signal and a ref here was dropped by the
  // transpiler without a diagnostic — a separate bug, reported with C23.)
  const typed = () => {
    return field.text === local() ? local() : "MISMATCH signal '" + local() + "' field '" + field.text + "'";
  };
  onMount(() => {
    uid = props.register(props.list, props.row.id, type, typed, () => props.row.label);
  });
  onCleanup(() => props.unregister(uid));
  return (
    <div class="row">
      <text class="label">{props.row.label}</text>
      <text class="local">{local()}</text>
      <input ref={field} />
    </div>
  );
}

export function ForIdentity() {
  const [plain, setPlain] = createSignal([]);
  const [keyed, setKeyed] = createSignal([]);
  // uid -> { list, id, set, get, label, alive }
  const live = {};
  const counts = { seq: 0, D: { mounts: 0, cleanups: 0 }, K: { mounts: 0, cleanups: 0 } };

  const born = (list, id, set, get, label) => {
    counts.seq = counts.seq + 1;
    live[counts.seq] = { list: list, id: id, set: set, get: get, label: label, alive: true };
    counts[list].mounts = counts[list].mounts + 1;
    return counts.seq;
  };
  const markDead = (r) => {
    r.alive = false;
    counts[r.list].cleanups = counts[r.list].cleanups + 1;
    return 1;
  };
  // (No `if` statement: the transpiler's inline-block emitter supports var/return/expression only.)
  const died = (uid) => (live[uid] ? markDead(live[uid]) : 0);

  const build = (n) => Array.from({ length: n }, (_, i) => ({ id: i + 1, label: "row " + (i + 1) }));
  const setList = (L, v) => (L === "D" ? setPlain(v) : setKeyed(v));
  const getList = (L) => (L === "D" ? plain() : keyed());
  // The live row instance (uid) showing `id` in list L; 0 when none, -1 when more than one.
  const uidOf = (L, id) => {
    const found = Object.keys(live).filter((u) => live[u].alive && live[u].list === L && live[u].id === id);
    return found.length === 1 ? Number(found[0]) : found.length === 0 ? 0 : -1;
  };
  const snapshot = (L) => getList(L).map((r) => uidOf(L, r.id));
  const report = (ok, name, detail) => console.log("C23 " + (ok ? "PASS " : "FAIL ") + name + " " + detail);
  const delta = (L, before) => ({ mounts: counts[L].mounts - before.mounts, cleanups: counts[L].cleanups - before.cleanups });
  const mark = (L) => ({ mounts: counts[L].mounts, cleanups: counts[L].cleanups });
  const expectCounts = (name, L, before, mounts, cleanups) => {
    const d = delta(L, before);
    report(d.mounts === mounts && d.cleanups === cleanups, name + "-" + L + "-counts",
      "mounts " + d.mounts + "/" + mounts + " cleanups " + d.cleanups + "/" + cleanups);
  };
  const sameUids = (name, L, before, skip) => {
    const after = snapshot(L);
    const bad = after.filter((u, i) => i !== skip && u !== before[i]);
    report(bad.length === 0 && after.every((u) => u > 0), name + "-" + L + "-others", "before " + before.join(",") + " after " + after.join(","));
  };

  const replacedD = (s, now) => {
    report(now > 0 && now !== s.old && !live[s.old].alive, "replace-D-recreated", "uid " + s.old + " -> " + now);
    report(now > 0 && live[now].get() === "", "replace-D-state-reset", "local '" + (now > 0 ? live[now].get() : "?") + "'");
    report(now > 0 && live[now].label() === "new four", "replace-D-label", now > 0 ? live[now].label() : "?");
    expectCounts("replace", "D", s.before, 1, 1);
  };
  const replacedK = (s, now) => {
    report(now === s.old && live[now].alive, "replace-K-reused", "uid " + s.old + " -> " + now);
    report(now > 0 && live[now].get() === "typed 4", "replace-K-state-kept", "local '" + (now > 0 ? live[now].get() : "?") + "'");
    report(now > 0 && live[now].label() === "new four", "replace-K-label", now > 0 ? live[now].label() : "?");
    expectCounts("replace", "K", s.before, 0, 0);
  };
  const refetchedD = (s) => {
    expectCounts("refetch", "D", s.before, s.n, s.n);
    report(snapshot("D").every((u, i) => u > 0 && u !== s.uids[i]), "refetch-D-all-new", snapshot("D").join(","));
  };
  const refetchedK = (s) => {
    expectCounts("refetch", "K", s.before, 0, 0);
    report(snapshot("K").join(",") === s.uids.join(","), "refetch-K-all-kept", snapshot("K").join(","));
  };

  // Each step: [name, act(L) -> state, check(L, state)]. Acts run on both lists; checks after a tick.
  const steps = () => [
    ["mount", (L) => ({ before: mark(L) }), (L, s) => {
      report(snapshot(L).every((u) => u > 0), "mount-" + L, snapshot(L).join(","));
      expectCounts("mount", L, { mounts: 0, cleanups: 0 }, 6, 0);
    }],
    ["type", (L) => {
      getList(L).forEach((r) => live[uidOf(L, r.id)].set("typed " + r.id));
      return { before: mark(L) };
    }, (L, s) => {
      report(getList(L).every((r) => live[uidOf(L, r.id)].get() === "typed " + r.id), "type-" + L, "locals set");
    }],
    // (a)/(b): a NEW object with the same id and a new label at position 3.
    ["replace", (L) => {
      const s = { before: mark(L), uids: snapshot(L), old: uidOf(L, 4) };
      const r = getList(L).slice();
      r[3] = { id: 4, label: "new four" };
      setList(L, r);
      return s;
    }, (L, s) => {
      (L === "D" ? replacedD : replacedK)(s, uidOf(L, 4));
      sameUids("replace", L, s.uids, 3);
    }],
    // (c): the SAME object mutated in place and re-sent in a new array: not re-read (5c), row kept.
    ["mutate", (L) => {
      const s = { before: mark(L), uids: snapshot(L), old: uidOf(L, 3) };
      getList(L)[2].label = "mutated";
      setList(L, getList(L).slice());
      return s;
    }, (L, s) => {
      report(uidOf(L, 3) === s.old && live[s.old].get() === "typed 3", "mutate-" + L + "-kept", "uid " + uidOf(L, 3));
      report(live[s.old].label() === "row 3", "mutate-" + L + "-not-reread", live[s.old].label());
      expectCounts("mutate", L, s.before, 0, 0);
      sameUids("mutate", L, s.uids, -1);
    }],
    // (d) swap rows 1 and 4 (same objects): every row instance and its state kept, nothing mounted.
    ["swap", (L) => {
      const s = { before: mark(L), byId: getList(L).map((r) => uidOf(L, r.id)) };
      const r = getList(L).slice();
      const t = r[1];
      r[1] = r[4];
      r[4] = t;
      setList(L, r);
      return s;
    }, (L, s) => {
      expectCounts("swap", L, s.before, 0, 0);
      const ids = getList(L).map((r) => r.id);
      report(ids[1] === 5 && ids[4] === 2 && uidOf(L, 2) === s.byId[1] && uidOf(L, 5) === s.byId[4], "swap-" + L + "-moved", ids.join(","));
      report(live[uidOf(L, 5)].get() === "typed 5", "swap-" + L + "-state-kept", live[uidOf(L, 5)].get());
    }],
    // (d) remove one: exactly its instance cleans up.
    ["remove", (L) => {
      const s = { before: mark(L), gone: uidOf(L, 6), uids: snapshot(L) };
      setList(L, getList(L).filter((r) => r.id !== 6));
      return s;
    }, (L, s) => {
      expectCounts("remove", L, s.before, 0, 1);
      report(!live[s.gone].alive, "remove-" + L + "-gone", "uid " + s.gone);
      report(snapshot(L).every((u) => s.uids.includes(u)), "remove-" + L + "-others", snapshot(L).join(","));
    }],
    // (d) reorder (reverse, same objects): nothing mounted or cleaned up.
    ["reverse", (L) => {
      const s = { before: mark(L), uids: snapshot(L) };
      setList(L, getList(L).slice().reverse());
      return s;
    }, (L, s) => {
      expectCounts("reverse", L, s.before, 0, 0);
      report(snapshot(L).join(",") === s.uids.slice().reverse().join(","), "reverse-" + L + "-instances", snapshot(L).join(","));
    }],
    // A FRESH copy of the whole list (same ids and labels, all new objects — e.g. a refetch):
    // default recreates every row, keyed keeps every row.
    ["refetch", (L) => {
      const s = { before: mark(L), uids: snapshot(L), n: getList(L).length };
      setList(L, JSON.parse(JSON.stringify(getList(L))));
      return s;
    }, (L, s) => (L === "D" ? refetchedD : refetchedK)(s)],
    // (d) clear: every instance cleans up.
    ["clear", (L) => {
      const s = { before: mark(L), n: getList(L).length };
      setList(L, []);
      return s;
    }, (L, s) => {
      expectCounts("clear", L, s.before, 0, s.n);
      report(Object.keys(live).every((u) => live[u].list !== L || !live[u].alive), "clear-" + L + "-none-alive", "");
    }],
  ];

  const run = (k) => {
    const all = steps();
    if (k >= all.length) {
      console.log("C23 done");
      return;
    }
    const step = all[k];
    const sD = step[1]("D");
    const sK = step[1]("K");
    setTimeout(() => {
      step[2]("D", sD);
      step[2]("K", sK);
      setTimeout(() => run(k + 1), 20);
    }, 30);
  };

  onMount(() => {
    setPlain(build(6));
    setKeyed(build(6));
    setTimeout(() => run(0), 100);
  });

  return (
    <div class="lists">
      <div class="list">
        <For each={plain()}>{(row) => <IdRow row={row} list="D" register={born} unregister={died} />}</For>
      </div>
      <div class="list">
        <For each={keyed()} key="id">{(row) => <IdRow row={row} list="K" register={born} unregister={died} />}</For>
      </div>
    </div>
  );
}
