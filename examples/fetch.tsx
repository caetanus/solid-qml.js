// Canonical Solid data-fetching pattern (createResource + a source signal + Suspense), as in
// the official Solid tutorial — only the JSX elements are mapped to the solid-qml runtime. Used
// to GAP-ANALYSE async data on solid-qml (it exercises createResource, Suspense, fetch, and the
// resource's loading/value accessors). The GitHub users API is public and reliable.
import { createSignal, createResource, Suspense, Show } from "solid-js";
import { div, text } from "../src/solid-qml/runtime";
import "./fetch.css";

type User = { name: string; login: string; bio: string; public_repos: number; avatar_url: string };

const fetchUser = async (login: string): Promise<User> =>
  (await fetch(`https://api.github.com/users/${login}`)).json();

export function Fetch() {
  const [login, setLogin] = createSignal("solidjs");
  const [user] = createResource(login, fetchUser);

  return (
    <div class="fetch-app">
      <text class="fetch-h1">github</text>

      <input
        class="fetch-search"
        placeholder="GitHub login"
        value={login()}
        onInput={(e) => setLogin(e.currentTarget.value)}
      />

      <Suspense fallback={<text class="fetch-loading">loading…</text>}>
        <Show when={user()} fallback={<text class="fetch-empty">no such user</text>}>
          <div class="fetch-card">
            <img class="fetch-avatar" src={user()!.avatar_url} />
            <text class="fetch-name">{user()!.name}</text>
            <text class="fetch-bio">{user()!.bio}</text>
            <text class="fetch-repos">{user()!.public_repos} public repos</text>
          </div>
        </Show>
      </Suspense>
    </div>
  );
}
