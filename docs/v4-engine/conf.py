# Sphinx configuration for the solid-qml-js V4 engine documentation.
# Build:  pip install -r requirements.txt && sphinx-build -b html . _build/html

project = "solid-qml-js — V4 Engine"
author = "solid-qml-native"
copyright = "2026, solid-qml-native"
release = "0.1.0"

extensions = [
    "myst_parser",  # author pages in Markdown
]

myst_enable_extensions = [
    "deflist",
    "colon_fence",
]

source_suffix = {
    ".md": "markdown",
    ".rst": "restructuredtext",
}

root_doc = "index"
exclude_patterns = ["_build", "Thumbs.db", ".DS_Store"]

html_theme = "alabaster"  # bundled with Sphinx; no extra dependency
html_title = "solid-qml-js V4 Engine"
html_static_path = []

# Keep code blocks readable even without Pygments lexers for QML.
highlight_language = "javascript"
