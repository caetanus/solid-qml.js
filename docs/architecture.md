# Arquitetura

solid-qml.js tem três camadas: o **transpiler**, a **engine de CSS em C++**, e os **shims de
browser** sobre o motor V4 do Qt.

## Transpiler (`transpiler/`)

Escrito em TypeScript. Normaliza o JSX do Solid para chamadas `h()` (via Babel), analisa os
símbolos reativos do componente (signals, effects, memos, resources, props) e emite QML
estrutural — **um arquivo `.qml` por componente**. Segue os imports para montar o grafo de
dependências e reproduzir a composição como imports QML.

Cobre reatividade (`createSignal`/`createEffect`/`createMemo`/`createResource`) e control flow
(`<Show>`, `<Switch>`/`<Match>` lazy, `<For>`, `<Index>`, `<Suspense>`), mapeando cada construção
para o equivalente nativo em QtQuick (bindings, `Repeater`, Loaders).

Imports de módulos npm/node são **espelhados**: o código real do pacote é reescrito para ESM
carregável pelo V4 e executado no engine — não é reimplementação.

## Engine de CSS em C++ (`subprojects/qml-css-engine/`)

Vendorizada. Faz **layout E paint**: box-model, flexbox, grid, `calc()`, `@media`, `vw`/`vh`,
`@font-face` remoto (download + cache + registro no `QFontDatabase`). Expõe primitivos leves
(`CssRect`, `CssText`, `CssFill`, …) sobre Item/Text/TextInput/MouseArea/Repeater — sem
`QtQuick.Controls`.

O hot path é o `CssLayoutEngine` (roda por relayout) e é escrito em C++ com zero overhead de
abstração — sem cópias, `std::function` ou virtuais supérfluos no caminho quente.

## Shims de browser sobre o V4 (`src/shims/`)

O V4 é o interpretador ECMAScript dentro do Qt/QML, sem DOM nem globais de browser. Os shims
instalam os globais que o código Solid espera:

- `fetch` — HTTPS real via `QNetworkAccessManager`, com `Headers`/`Request`/`Response`/
  `AbortController`.
- `localStorage` — persistente em disco.
- `XMLHttpRequest` — sobre o mesmo transporte de rede.
- timers (`setTimeout`/`setInterval`) e polyfills de JS ausentes no V4.

O `loader` (`src/loader.cpp`) é o executável que carrega o QML gerado, instala os shims e a folha
de CSS, e observa arquivos para hot-reload.
