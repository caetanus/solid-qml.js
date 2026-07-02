# qml-css-engine

Real CSS for Qt Quick — layout **and** paint driven by a native CSS engine.

`qml-css-engine` parses a standard CSS stylesheet and applies it to Qt Quick items.
A C++ layout engine handles flexbox, grid, and the full box model; a set of QML
components renders fills, gradients, shadows, transitions, and animations — all from the
same `.css` file.

```sh
# Build these docs
pip install -r requirements.txt
sphinx-build -b html docs docs/_build/html
```

```{toctree}
:maxdepth: 2
:caption: Contents

overview
integration
components
css-support
cpp-api
```
