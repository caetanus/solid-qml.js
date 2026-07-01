# Getting started

## Requisitos

- Node.js + npm
- **Qt 6** (Core, Gui, Qml, Quick, Test)
- **Meson** e **Ninja**

## Instalação e build

```sh
# 1. dependências JS
npm install

# 2. build do loader nativo (a engine de CSS em C++ vem vendorizada em subprojects/)
meson setup build
ninja -C build
```

## Dev nativo

```sh
node --import tsx scripts/dev-native.mjs
```

Isso transpila `src/mainqml.tsx` para QML (`qml/solidqml/App.generated.qml` + um `.qml` por
componente + `App.generated.css`) e abre a janela nativa com hot-reload: o loop observa `src/` e
`examples/`, regenera o QML a cada mudança, e o loader recarrega a cena ao vivo.

## Outros comandos

```sh
npm run test:transpiler   # suíte do transpiler (node --test)
npm run dev               # preview web via Vite (Solid rodando no browser)
```

Veja o `package.json` para a lista atual de scripts.
