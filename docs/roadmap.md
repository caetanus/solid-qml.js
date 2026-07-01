# Roadmap & Gaps

## Roadmap

- **Uma única codebase para todas as plataformas** — Android, iOS, Windows, macOS e Linux
  (mobile **e** desktop) a partir do mesmo código Solid.
- **AOT → C++ no release** — o gerador emite QtQuick C++ 1:1, eliminando o V4 interpretado; o
  V4-AOT do Qt cobre só o JS dinâmico residual (async/fetch/closures).
- **Interop com C++ nativo real** ao lado dos componentes Solid.
- **Importar componentes QML reais** (interop / código legado).
- **Packaging decente** para deploy, e caminho para **exportar para projetos legados**.

## Gaps / Limitações atuais

Alpha — sendo honestos sobre o que **ainda não existe**:

- **Integração desktop faltando**: D-Bus, Avahi/zeroconf e system tray ainda não.
- **Threads** e **background services (mobile)** ainda não.
- **Gestures** (touch) ainda não.
- **Scroll** de overflow ainda não — o QtQuick oferece via `Flickable`; está no topo da fila.
- `<For>` ainda é frágil com **sub-JSX aninhado** e casos complexos.
- Mapeamentos de **CSS avançado** dependem de decisões de design abertas.
- **AOT → C++ ainda não implementado** — hoje o dev roda no V4 interpretado.
- **Packaging com Qt é doloroso** — o sistema de deploy ainda está em aberto.
