<p align="center">
  <img src="assets/logo.png" width="132" alt="solid-qml.js logo">
</p>

<h1 align="center">solid-qml.js</h1>

<p align="center">
  <strong>Solid.js, renderizado nativo via QML/QtQuick — sem browser.</strong>
</p>

<p align="center">
  <em>⚠️ Experimental / alpha — APIs, formatos gerados e comandos ainda mudam sem aviso.</em>
</p>

---

## O que é

**solid-qml.js** transpila componentes [Solid.js](https://www.solidjs.com/) (JSX + CSS) para
**QtQuick nativo**: uma scene-graph de verdade, renderizada pela GPU via Qt — **não** uma webview,
**não** um DOM emulado.

Você escreve UI em TypeScript/JSX com o modelo reativo do Solid (signals, effects, memos, control
flow) e estiliza com CSS comum. O transpiler segue os imports e emite **um arquivo `.qml` por
componente**, mais uma folha de estilo, que rodam sobre uma engine de CSS própria em C++.

- **vs Electron** — sem empacotar Chromium + Node em cada app. O runtime é o Qt, a UI é
  scene-graph nativo, o footprint é uma fração.
- **vs React Native** — componentes nativos reais, sem ponte JS assíncrona entre mundos. E, no
  release, o alvo é compilar tudo para **C++ (AOT)**, eliminando o interpretador.

## Destaques (reais, funcionam hoje)

- ⭐ **Imports de módulos npm/node rodando no motor V4 do Qt.** O maior defeito histórico do QML
  sempre foi não poder importar pacotes do npm — resolvemos isso. O **código real do pacote** é
  espelhado e executado de verdade no engine V4 (não é reimplementação nem stub); cada pacote que
  quebra revela um buraco do V4 que a gente tapa.
- **Engine de CSS própria em C++** que faz **layout E paint**: box-model, **flexbox**, **grid**,
  `calc()`, `@media`, unidades `vw`/`vh`. Sem `QtQuick.Controls` — só primitivos leves
  (Item / Text / TextInput / MouseArea / Repeater). O hot path (relayout) é C++ com zero overhead
  de abstração.
- **Web fonts via `@font-face` remoto** — baixa a fonte, cacheia e registra no `QFontDatabase` em
  runtime. Não instala nada no sistema.
- **Reatividade do Solid → QML**: `createSignal`, `createEffect`, `createMemo`, `createResource` +
  `<Suspense>`, `<Show>`, `<Switch>`/`<Match>` (lazy), `<For>`, `<Index>`.
- **Shims de browser sobre o V4**: `fetch` (HTTPS real via `QNetworkAccessManager`, com
  `Headers`/`Request`/`Response`/`AbortController`), `localStorage` (persistente),
  `XMLHttpRequest`, timers.
- **Responsivo** — o layout reflui no resize da janela; `@media`, `vw`, `vh` reavaliam ao vivo.

## Quickstart

Requisitos: Node.js + npm, **Qt 6** (Core, Gui, Qml, Quick, Test), **Meson** e **Ninja**.

```sh
# 1. dependências JS
npm install

# 2. build do loader nativo (a engine de CSS em C++ vem vendorizada em subprojects/)
meson setup build
ninja -C build

# 3. dev nativo: transpila src/mainqml.tsx → QML e abre a janela nativa com hot-reload
node --import tsx scripts/dev-native.mjs
```

O loop de dev observa `src/` e `examples/`, regenera o QML a cada mudança e o loader recarrega a
cena ao vivo (edite o CSS e veja o restyle na hora).

Outros comandos úteis (veja `package.json`):

```sh
npm run test:transpiler   # suíte do transpiler (node --test)
npm run dev               # preview web via Vite (Solid rodando no browser)
```

## Roadmap

- **Uma única codebase para todas as plataformas** — apps para **Android, iOS, Windows, macOS e
  Linux** (mobile **e** desktop) a partir do mesmo código Solid.
- **AOT → C++ no release** — nosso gerador emite QtQuick C++ 1:1, eliminando o V4 interpretado; o
  V4-AOT do Qt cobre só o JS dinâmico residual (async/fetch/closures).
- **Interop com C++ nativo real** ao lado dos componentes Solid.
- **Importar componentes QML reais** (interop / código legado).
- **Packaging decente** para deploy, e caminho para **exportar para projetos legados**.

## Gaps / Limitações atuais

Estamos em alpha e fazemos questão de ser honestos sobre o que **ainda não existe**:

- **Integração desktop faltando**: D-Bus, Avahi/zeroconf e system tray ainda não.
- **Threads** e **background services (mobile)** ainda não.
- **Gestures** (touch) ainda não.
- **Scroll** de overflow ainda não — o QtQuick oferece via `Flickable`; está no topo da fila.
- `<For>` ainda é frágil com **sub-JSX aninhado** e casos complexos.
- Mapeamentos de **CSS avançado** dependem de decisões de design abertas.
- **AOT → C++ ainda não implementado** — hoje o dev roda no V4 interpretado.
- **Packaging com Qt é doloroso** — o sistema de deploy ainda está em aberto.

## Licença

TBD (experimental).
