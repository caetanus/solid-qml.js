project(
  'solid-qml-native',
  'cpp',
  version: '0.1.0',
  license: 'MIT',
  # The CSS engine lives in vendor/ as a git submodule (its own repo, shared with other
  # projects) — meson resolves subproject('qml-css-engine') against this directory.
  subproject_dir: 'vendor',
  default_options: [
    'cpp_std=c++20',
    'warning_level=2',
  ],
)

qt6 = import('qt6')

# The project version, visible to app code as process.versions.solidQml (Electron idiom).
add_project_arguments('-DSOLID_QML_VERSION="' + meson.project_version() + '"', language: 'cpp')

# Compile in QML/JS debugging (the V4 debug server, activated at runtime by
# -qmljsdebugger=...). Dev builds only; the server only listens when the arg is passed.
if get_option('qml_debug')
  add_project_arguments('-DQT_QML_DEBUG', language: 'cpp')
endif

# The CSS engine (parse/cascade/paint + C++ layout) is its own repo, consumed as a meson
# subproject (git submodule under subprojects/). Shared with qbar.
qmlcss = subproject('qml-css-engine')
qml_css_engine_dep = qmlcss.get_variable('qml_css_engine_dep')

# fetch() shim transport.
qt6_network_dep = dependency('qt6', modules: ['Network'])

# Qt Widgets: required by Qt Labs Platform for OS-native menu bars / menus / dialogs (the platform
# reports "Qt Labs Platform requires Qt Widgets" without it). The app uses a QApplication so the
# native <MenuBar> attaches to the window chrome (owner call 2026-07-07: accept the footprint).
qt6_widgets_dep = dependency('qt6', modules: ['Widgets'])

# Desktop notifications backend (Linux): org.freedesktop.Notifications over D-Bus, on a worker
# thread (the qbar pattern — QDBusConnection is synchronous). Optional: without it the
# notifications shim compiles as unavailable; other platforms get their own backends later.
qt6_dbus_dep = dependency('qt6', modules: ['DBus'], required: false)
notify_args = qt6_dbus_dep.found() ? ['-DHAVE_QTDBUS'] : []

# The <RichText> module's ODF loader uses the private QZipReader (Qt ships no public unzip;
# this reader is how Qt itself opens .odt content).
qt6_core_private_dep = dependency('qt6', modules: ['Core'], private_headers: true)


# No qresources: the generated app (App.generated.qml + its .js sidecar) and the stylesheet are
# loaded from the filesystem at runtime, not baked into the binary. Release uses the AOT C++
# back-end instead of qrc-embedded QML.
solid_qml_loader_qt_sources = qt6.preprocess(
  moc_headers: files(
    'src/shims/weblocalstorage.h',
    'src/shims/webfetch.h',
    'src/shims/webtimers.h',
    'src/shims/webplatform.h',
    'src/shims/nodeshims.h',
    'src/shims/tabstop.h',
    'src/shims/notifications.h',
    'src/shims/richtext.h',
    'src/shims/codeeditor.h',
    'src/widgets/primitives.h',
    'src/widgets/focusring.h',
    'src/widgets/button.h',
    'src/widgets/indicators.h',
    'src/widgets/toggles.h',
    'src/widgets/sliders.h',
  ),
)

executable(
  'solid-qml-loader',
  files(
    'src/loader.cpp',
    'src/shims/weblocalstorage.cpp',
    'src/shims/webfetch.cpp',
    'src/shims/webtimers.cpp',
    'src/shims/webplatform.cpp',
    'src/shims/jspolyfill.cpp',
    'src/shims/nodeshims.cpp',
    'src/shims/notifications.cpp',
    'src/shims/richtext.cpp',
    'src/shims/codeeditor.cpp',
    'src/widgets/solidwidgets.cpp',
    'src/widgets/focusring.cpp',
    'src/widgets/button.cpp',
    'src/widgets/indicators.cpp',
    'src/widgets/toggles.cpp',
    'src/widgets/sliders.cpp',
    'src/widgets/snippetwidget.cpp',
  ),
  solid_qml_loader_qt_sources,
  cpp_args: notify_args,
  dependencies: [qml_css_engine_dep, qt6_network_dep, qt6_widgets_dep, qt6_dbus_dep, qt6_core_private_dep],
  install: true,
)
