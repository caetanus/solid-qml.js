project = "qml-css-engine"
copyright = "qml-css-engine contributors"
author = "qml-css-engine contributors"
release = "0.1.0"

extensions = ["myst_parser"]

source_suffix = {".md": "markdown"}
root_doc = "index"

# Theme: prefer furo (modern, readable). If it is not installed, fall back to
# alabaster (ships with Sphinx). Install furo with: pip install furo
try:
    import furo  # noqa: F401
    html_theme = "furo"
except ImportError:
    html_theme = "alabaster"

html_title = "qml-css-engine"
