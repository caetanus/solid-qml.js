project(
  'qml-css-engine',
  'cpp',
  version: '0.1.0',
  license: 'MIT',
  default_options: [
    'cpp_std=c++20',
    'warning_level=2',
  ],
)

qt6 = import('qt6')

qt6_dep = dependency(
  'qt6',
  modules: ['Core', 'Gui', 'Qml', 'Quick', 'Network'],
)

qml_css_engine_sources = files(
  'src/qmlcss/csstheme.cpp',
  'src/qmlcss/csslayout.cpp',
  'src/qmlcss/valueparser.cpp',
)

qml_css_engine_headers = files(
  'src/qmlcss/csstheme.h',
  'src/qmlcss/csslayout.h',
  'src/qmlcss/valueparser.h',
)

qml_css_engine_qt_sources = qt6.preprocess(
  moc_headers: files('src/qmlcss/csstheme.h', 'src/qmlcss/csslayout.h'),
  qresources: files('qml/qmlcss.qrc'),
)

qml_css_engine_include = include_directories('src')

qml_css_engine_lib = static_library(
  'qmlcssengine',
  qml_css_engine_sources,
  qml_css_engine_qt_sources,
  include_directories: qml_css_engine_include,
  dependencies: qt6_dep,
  install: true,
)

# Consumers (e.g. the solid-qml app, or qbar) link this and get the qmlcss/ headers + the
# qrc:/qmlcss QML resources. Exposed as a meson subproject variable: `qml_css_engine_dep`.
qml_css_engine_dep = declare_dependency(
  link_with: qml_css_engine_lib,
  include_directories: qml_css_engine_include,
  dependencies: qt6_dep,
  sources: qml_css_engine_qt_sources,
)

install_headers(qml_css_engine_headers, subdir: 'qmlcss')

# Tests run only when this is built on its own (not when consumed as a subproject).
if not meson.is_subproject()
  qt6_test_dep = dependency(
    'qt6',
    modules: ['Core', 'Gui', 'Qml', 'Quick', 'Test'],
  )

  css_test_moc = qt6.preprocess(moc_headers: files('tests/test_css.h'))
  css_test = executable(
    'test-css',
    files('tests/test_css.cpp'),
    css_test_moc,
    dependencies: [qml_css_engine_dep, qt6_test_dep],
  )
  test('css', css_test)

  qml_css_test_moc = qt6.preprocess(moc_headers: files('tests/test_qml_css.h'))
  qml_test = executable(
    'test-qml-css',
    files('tests/test_qml_css.cpp'),
    qml_css_test_moc,
    dependencies: [qml_css_engine_dep, qt6_test_dep],
  )
  test('qml-css', qml_test, env: ['QT_QPA_PLATFORM=offscreen'])
endif
