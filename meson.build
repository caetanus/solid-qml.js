project(
  'solid-qml-native',
  'cpp',
  version: '0.1.0',
  license: 'MIT',
  default_options: [
    'cpp_std=c++20',
    'warning_level=2',
  ],
)

qt6 = import('qt6')

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
  ),
  solid_qml_loader_qt_sources,
  dependencies: [qml_css_engine_dep, qt6_network_dep],
  install: true,
)
