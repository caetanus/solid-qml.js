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

# TabButton/Menu/MenuItem/MenuSeparator: their containers (QQuickTabBar/QQuickMenu) manage the
# children through qobject_cast to these PRIVATE Templates classes, so the C++ widgets must
# subclass them (same private-header precedent as QZipReader; ABI follows the installed Qt).
qt6_templates_private_dep = dependency('qt6', modules: ['Core', 'Gui', 'Qml', 'QmlModels', 'Quick', 'QuickTemplates2'], private_headers: true)


# No qresources: the generated app (App.generated.qml + its .js sidecar) and the stylesheet are
# loaded from the filesystem at runtime, not baked into the binary. Release uses the AOT C++
# back-end instead of qrc-embedded QML.
solid_qml_loader_qt_sources = qt6.preprocess(
  moc_headers: files(
    'src/embed/solidqmlembed.h',
    'src/shims/weblocalstorage.h',
    'src/shims/webfetch.h',
    'src/shims/webtimers.h',
    'src/shims/backgroundtasks.h',
    'src/shims/sharedbuffers.h',
    'src/shims/webworker.h',
    'src/shims/webplatform.h',
    'src/shims/nodeshims.h',
    'src/shims/tabstop.h',
    'src/shims/notifications.h',
    'src/shims/richtext.h',
    'src/shims/codeeditor.h',
    'src/widgets/listview.h',
    'src/widgets/menuwidgets.h',
    'src/widgets/monthgrid.h',
    'src/widgets/primitives.h',
    'src/widgets/focusring.h',
    'src/widgets/button.h',
    'src/widgets/indicators.h',
    'src/widgets/toggles.h',
    'src/widgets/select.h',
    'src/widgets/sliders.h',
    'src/widgets/calendar.h',
    'src/widgets/containerbase.h',
    'src/widgets/datefield.h',
    'src/widgets/delaybutton.h',
    'src/widgets/details.h',
    'src/widgets/dialog.h',
    'src/widgets/drawer.h',
    'src/widgets/spindial.h',
    'src/widgets/splitview.h',
    'src/widgets/swipeview.h',
    'src/widgets/tabbar.h',
    'src/widgets/tabbutton.h',
    'src/widgets/tableview.h',
    'src/widgets/textinputs.h',
    'src/widgets/toolbar.h',
    'src/widgets/tray.h',
    'src/widgets/treeview.h',
    'src/widgets/tumbler.h',
  ),
)

# ─── libsolidqml: the embeddable runtime (owner 2026-07-22: host pre-existing C++/QtQuick
# apps). Everything except the loader main: widgets, shims, the embed surface. The loader links
# it; external QtQuick apps consume it via pkg-config (`solidqml`) or CMake
# (find_package(SolidQml)) and call SolidQmlEmbed::init(engine).
solidqml_lib = shared_library(
  'solidqml',
  files(
    'src/shims/weblocalstorage.cpp',
    'src/shims/webfetch.cpp',
    'src/shims/webtimers.cpp',
  'src/shims/backgroundtasks.cpp',
  'src/shims/sharedbuffers.cpp',
  'src/shims/webworker.cpp',
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
    'src/widgets/listview.cpp',
    'src/widgets/menuwidgets.cpp',
    'src/widgets/monthgrid.cpp',
    'src/widgets/select.cpp',
    'src/widgets/sliders.cpp',
    'src/widgets/calendar.cpp',
    'src/widgets/containerbase.cpp',
    'src/widgets/datefield.cpp',
    'src/widgets/delaybutton.cpp',
    'src/widgets/details.cpp',
    'src/widgets/dialog.cpp',
    'src/widgets/drawer.cpp',
    'src/widgets/spindial.cpp',
    'src/widgets/splitview.cpp',
    'src/widgets/swipeview.cpp',
    'src/widgets/tabbar.cpp',
    'src/widgets/tabbutton.cpp',
    'src/widgets/tableview.cpp',
    'src/widgets/textinputs.cpp',
    'src/widgets/toolbar.cpp',
    'src/widgets/tray.cpp',
    'src/widgets/treeview.cpp',
    'src/widgets/tumbler.cpp',
    'src/widgets/snippetwidget.cpp',
    'src/embed/solidqmlembed.cpp',
  ),
  solid_qml_loader_qt_sources,
  cpp_args: notify_args,
  include_directories: include_directories('src'),
  dependencies: [qml_css_engine_dep, qt6_network_dep, qt6_widgets_dep, qt6_dbus_dep, qt6_core_private_dep, qt6_templates_private_dep],
  version: '0.2.0',
  install: true,
)

solidqml_dep = declare_dependency(
  link_with: solidqml_lib,
  include_directories: include_directories('src'),
  dependencies: [qml_css_engine_dep],
)

install_headers('src/embed/solidqmlembed.h', subdir: 'solidqml')

pkg = import('pkgconfig')
pkg.generate(
  solidqml_lib,
  name: 'solidqml',
  description: 'solid-qml embeddable runtime (CSS engine, widgets, shims, SolidIsland)',
  subdirs: 'solidqml',
  requires: ['Qt6Quick'],
)

cmake = import('cmake')
cmake.write_basic_package_version_file(name: 'SolidQml', version: '0.2.0')
cmake.configure_package_config_file(
  name: 'SolidQml',
  input: 'cmake/SolidQmlConfig.cmake.in',
  configuration: configuration_data(),
)

executable(
  'solid-qml-loader',
  files('src/loader.cpp'),
  cpp_args: notify_args,
  include_directories: include_directories('src'),
  dependencies: [solidqml_dep, qt6_network_dep, qt6_widgets_dep, qt6_dbus_dep, qt6_core_private_dep, qt6_templates_private_dep],
  install: true,
)


# Demo: a vanilla QtQuick app hosting a solid-qml island (Direção A of the embed plan).
executable(
  'embed-host-demo',
  files('examples/embed-host/main.cpp'),
  cpp_args: notify_args + [
    '-DSOLIDQML_APP_DIR="' + meson.current_source_dir() + '/qml/solidqml/"',
    '-DEMBED_HOST_QML="' + meson.current_source_dir() + '/examples/embed-host/main.qml"',
  ],
  include_directories: include_directories('src'),
  dependencies: [solidqml_dep, qt6_widgets_dep],
)


# Demo: "legacy" C++ shipped as a canonical QML extension plugin, consumed from TSX via
# `import { AcmeGauge } from "qml:Acme"` + loader --import-path (Direção B of the embed plan).
acme_qt_dep = dependency('qt6', modules: ['Quick'])
acme_moc = qt6.preprocess(moc_sources: files('examples/legacy-plugin/acmeplugin.cpp'), dependencies: [acme_qt_dep])
acme_plugin = shared_module(
  'acmeplugin',
  files('examples/legacy-plugin/acmeplugin.cpp'),
  acme_moc,
  dependencies: [acme_qt_dep],
)
custom_target(
  'acme-qml-module',
  input: ['examples/legacy-plugin/qmldir'],
  output: 'acme-module-stamp',
  command: [
    'sh', '-c',
    'mkdir -p @OUTDIR@/qmlmods/Acme && cp @INPUT0@ @OUTDIR@/qmlmods/Acme/ && cp ' +
    meson.current_build_dir() + '/libacmeplugin.so @OUTDIR@/qmlmods/Acme/ && touch @OUTPUT@',
  ],
  depends: acme_plugin,
  build_by_default: true,
)


# solidterm — the real, installable terminal app (owner 2026-07-22). Its own C++ (pty +
# libvterm TerminalView) registered as the SolidTerm module, UI in solid TSX, hosted via the
# embed surface. Dev app dir = apps/solidterm/qml (regenerate with:
#   node --import tsx scripts/gen.mjs apps/solidterm/src/mainqml.tsx apps/solidterm/qml/App.generated.qml).
vterm_dep = dependency('vterm')
util_dep = meson.get_compiler('cpp').find_library('util', required: false)
solidterm_moc = qt6.preprocess(
  moc_headers: files('apps/solidterm/native/ptysession.h', 'apps/solidterm/native/terminalview.h', 'apps/solidterm/native/systemtheme.h'),
)
executable(
  'solidterm',
  files(
    'apps/solidterm/main.cpp',
    'apps/solidterm/native/ptysession.cpp',
    'apps/solidterm/native/terminalview.cpp',
    'apps/solidterm/native/systemtheme.cpp',
  ),
  solidterm_moc,
  cpp_args: notify_args + ['-DSOLIDTERM_DEV_DIR="' + meson.current_source_dir() + '/apps/solidterm/qml"'],
  include_directories: include_directories('src', 'apps/solidterm'),
  dependencies: [solidqml_dep, qt6_widgets_dep, vterm_dep, util_dep],
  install: true,
)
