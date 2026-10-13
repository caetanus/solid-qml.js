project(
  'solid-qml-native',
  'cpp',
  # The single source of truth for the app's version (docs/release-policy.md). It may carry a
  # "-preview.N" prerelease suffix meson/pkg-config accept as free text but CMake's generated
  # package-version file does not (it wants a numeric major[.minor[.patch]]) — the CMake config
  # below is given a numeric-only version instead; see docs/release-policy.md for the split.
  version: '0.3.0-preview.1',
  # The framework is MIT; the CSS engine (vendor/qml-css-engine) is LGPL-3.0-only — see README.
  license: 'MIT',
  license_files: ['LICENSE'],
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
# subproject (git submodule under vendor/). Shared with other projects. `tests=enabled` makes the
# engine's css/qml-css/layout suites register into this project's `meson test`.
qmlcss = subproject('qml-css-engine', default_options: ['tests=enabled'])
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

# <RichText> feature (#4): its ODF loader uses the private QZipReader (Qt ships no public unzip;
# this reader is how Qt itself opens .odt content). Pulled ONLY when the widget is built, so a build
# with -Drichtext=disabled drops this private-header dependency. HAVE_RICHTEXT gates the source and
# the loader's type registration.
build_richtext = not get_option('richtext').disabled()
richtext_args = build_richtext ? ['-DHAVE_RICHTEXT'] : []
core_private_deps = build_richtext ? [dependency('qt6', modules: ['Core'], private_headers: true)] : []

# TabButton/Menu/MenuItem/MenuSeparator: their containers (QQuickTabBar/QQuickMenu) manage the
# children through qobject_cast to these PRIVATE Templates classes, so the C++ widgets must
# subclass them (same private-header precedent as QZipReader; ABI follows the installed Qt).
qt6_templates_private_dep = dependency('qt6', modules: ['Core', 'Gui', 'Qml', 'QmlModels', 'Quick', 'QuickTemplates2'], private_headers: true)


# No qresources: the generated app (App.generated.qml + its .js sidecar) and the stylesheet are
# loaded from the filesystem at runtime, not baked into the binary. Release uses the AOT C++
# back-end instead of qrc-embedded QML.
solidqml_moc_headers = files(
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
    'src/shims/codeeditor.h',
    'src/widgets/listview.h',
    'src/widgets/menuwidgets.h',
    'src/widgets/monthgrid.h',
    'src/widgets/primitives.h',
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
    'src/widgets/swipe.h',
    'src/widgets/dialog.h',
    'src/widgets/drawer.h',
    'src/widgets/tooltip.h',
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
)
if build_richtext
  solidqml_moc_headers += files('src/shims/richtext.h')
endif

# The generated app (App.generated.qml + its .js sidecar) and its stylesheet are loaded from the
# filesystem at runtime. What the SDK itself ships — the user-agent base.css and the opt-in widget
# modules — is baked into libsolidqml, so an installed host needs no data dir or import path.
solid_qml_loader_qt_sources = qt6.preprocess(
  moc_headers: solidqml_moc_headers,
  qresources: files('src/solid-qml/base.qrc', 'qml/solidqml/Widgets/widgets.qrc'),
)

# ─── libsolidqml: the embeddable runtime (owner 2026-07-22: host pre-existing C++/QtQuick
# apps). Everything except the loader main: widgets, shims, the embed surface. The loader links
# it; external QtQuick apps consume it via pkg-config (`solidqml`) or CMake
# (find_package(SolidQml)) and call SolidQmlEmbed::init(engine).
solidqml_sources = files(
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
    'src/shims/codeeditor.cpp',
    'src/widgets/a11y.cpp',
    'src/widgets/solidwidgets.cpp',
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
    'src/widgets/swipe.cpp',
    'src/widgets/dialog.cpp',
    'src/widgets/drawer.cpp',
    'src/widgets/tooltip.cpp',
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
)
if build_richtext
  solidqml_sources += files('src/shims/richtext.cpp', 'src/shims/richtext_styles.cpp', 'src/shims/richtext_odf.cpp', 'src/shims/richtext_qt_attach.cpp', 'src/shims/richtext_layout.cpp')
endif

# `embed` gates INSTALLATION as a consumable product; the library is always built because the
# loader/examples/solidterm all link it.
install_lib = get_option('embed').allowed()

solidqml_lib = shared_library(
  'solidqml',
  solidqml_sources,
  solid_qml_loader_qt_sources,
  cpp_args: notify_args + richtext_args,
  include_directories: include_directories('src'),
  dependencies: [qml_css_engine_dep, qt6_network_dep, qt6_widgets_dep, qt6_dbus_dep, qt6_templates_private_dep] + core_private_deps,
  # SONAME/ABI policy for 0.x (docs/release-policy.md): soversion stays '0' for the whole 0.x
  # series (no ABI stability promise pre-1.0) and `version` is the numeric libsolidqml.so.X.Y.Z
  # filename suffix — decoupled from the project's "-preview.N" marketing version above. This
  # library also links against Qt's private Templates headers, so it is built for exactly one Qt
  # minor (6.11) regardless of soversion; rebuild on any Qt or solid-qml upgrade.
  version: '0.3.0',
  soversion: '0',
  install: install_lib,
)

solidqml_dep = declare_dependency(
  link_with: solidqml_lib,
  include_directories: include_directories('src'),
  dependencies: [qml_css_engine_dep],
)

# `embed` feature: install the library as a consumable product (headers + pkg-config + CMake).
if install_lib
  install_headers('src/embed/solidqmlembed.h', subdir: 'solidqml')

  pkg = import('pkgconfig')
  pkg.generate(
    solidqml_lib,
    name: 'solidqml',
    description: 'solid-qml embeddable runtime (CSS engine, widgets, shims, SolidIsland)',
    subdirs: 'solidqml',
    version: meson.project_version(),
    requires: ['Qt6Quick'],
  )

  cmake = import('cmake')
  # CMake's generated version-check file requires a numeric major[.minor[.patch]] — it cannot
  # parse the "-preview.N" suffix in meson.project_version() (see docs/release-policy.md).
  cmake.write_basic_package_version_file(name: 'SolidQml', version: '0.3.0')
  cmake.configure_package_config_file(
    name: 'SolidQml',
    input: 'cmake/SolidQmlConfig.cmake.in',
    configuration: configuration_data(),
  )
endif

# `loader` feature: the standalone dev/preview runner.
if get_option('loader').allowed()
  loader_exe = executable(
    'solid-qml-loader',
    files('src/loader.cpp'),
    cpp_args: notify_args + richtext_args,
    include_directories: include_directories('src'),
    dependencies: [solidqml_dep, qt6_network_dep, qt6_widgets_dep, qt6_dbus_dep, qt6_templates_private_dep] + core_private_deps,
    install_rpath: '$ORIGIN/../' + get_option('libdir'),
    install: true,
  )
endif


# `examples` feature: the embed demos.
if get_option('examples').allowed()
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
endif


# `aot` feature — the C++ codegen (AOT) release path: the transpiler emits QtQuick C++ 1:1 instead
# of interpreted QML, dropping V4 from the hot path. M-AOT-0 is the counter proof (a hand-written
# scene that must render pixel-identical to the QML back-end; the emitter reproduces this pattern).
if get_option('aot').allowed()
  # The real deliverable: the counter compiled from generated C++ (transpiler cpp back-end).
  # Regenerate with: node --import tsx scripts/gen-cpp.mjs examples/aot/counter.tsx examples/aot/generated
  aot_gen_moc = qt6.preprocess(moc_headers: files('examples/aot/generated/generated.h'))
  executable(
    'aot-counter',
    files('examples/aot/generated/main.cpp', 'examples/aot/generated/generated.cpp'),
    aot_gen_moc,
    include_directories: include_directories('src'),
    dependencies: [solidqml_dep, qt6_widgets_dep],
  )

  # The hand-written proof the emitter was derived from — kept building so it stays honest.
  aot_proof_moc = qt6.preprocess(moc_headers: files('examples/aot/proof/counterapp.h'))
  executable(
    'aot-counter-proof',
    files('examples/aot/proof/main.cpp', 'examples/aot/proof/counterapp.cpp'),
    aot_proof_moc,
    include_directories: include_directories('src'),
    dependencies: [solidqml_dep, qt6_widgets_dep],
  )
endif


# `solidterm` feature — the real, installable terminal app (owner 2026-07-22). Its own C++ (pty +
# libvterm TerminalView) registered as the SolidTerm module, UI in solid TSX, hosted via the embed
# surface. libvterm is required only when solidterm is actually built, so a build on a platform
# without vterm (or with -Dsolidterm=disabled) configures cleanly — a Linux example no longer blocks
# the framework elsewhere. On `auto`, an absent vterm just skips the app. Dev app dir =
# apps/solidterm/qml (regenerate: node --import tsx scripts/gen.mjs apps/solidterm/src/mainqml.tsx …).
solidterm_opt = get_option('solidterm')
# Only probe libvterm when solidterm is not turned off — a disabled build never touches vterm.
build_solidterm = false
if not solidterm_opt.disabled()
  vterm_dep = dependency('vterm', required: solidterm_opt.enabled())
  build_solidterm = vterm_dep.found()
endif
if build_solidterm
  util_dep = meson.get_compiler('cpp').find_library('util', required: false)
  # Background blur: SolidTerm asks the COMPOSITOR to blur what shows through its translucent window,
  # via the standard `ext-background-effect-v1` (wayland-protocols staging). Needs wayland-client,
  # wayland-scanner, the protocol XML and Qt's private QWaylandWindow interface (the window's
  # wl_surface); without any of them the blur option is simply unavailable.
  wlclient_dep = dependency('wayland-client', required: false)
  wlscanner = find_program('wayland-scanner', required: false)
  wlprotocols_dep = dependency('wayland-protocols', version: '>= 1.45', required: false)
  blur_src = []
  blur_deps = []
  blur_args = []
  if wlclient_dep.found() and wlscanner.found() and wlprotocols_dep.found() and add_languages('c', native: false, required: false)
    blur_xml = wlprotocols_dep.get_variable('pkgdatadir') / 'staging/ext-background-effect/ext-background-effect-v1.xml'
    # Our own strength extension (also in the owner's sway): scales the blur where offered.
    strength_xml = files('apps/solidterm/protocols/background-effect-strength-unstable-v1.xml')
    blur_src = [
      custom_target('background-effect-client-header', input: blur_xml,
        output: 'ext-background-effect-v1-client-protocol.h',
        command: [wlscanner, 'client-header', '@INPUT@', '@OUTPUT@']),
      custom_target('background-effect-private-code', input: blur_xml,
        output: 'ext-background-effect-v1-protocol.c',
        command: [wlscanner, 'private-code', '@INPUT@', '@OUTPUT@']),
      custom_target('blur-strength-client-header', input: strength_xml,
        output: 'background-effect-strength-unstable-v1-client-protocol.h',
        command: [wlscanner, 'client-header', '@INPUT@', '@OUTPUT@']),
      custom_target('blur-strength-private-code', input: strength_xml,
        output: 'background-effect-strength-unstable-v1-protocol.c',
        command: [wlscanner, 'private-code', '@INPUT@', '@OUTPUT@']),
      files('apps/solidterm/native/windowblur.cpp'),
    ]
    blur_deps = [wlclient_dep, dependency('qt6', modules: ['Core', 'Gui'], private_headers: true)]
    blur_args = ['-DHAVE_BACKGROUND_EFFECT']
  endif
  solidterm_moc = qt6.preprocess(
    moc_headers: (blur_src.length() > 0 ? files('apps/solidterm/native/windowblur.h') : []) + files('apps/solidterm/native/ptysession.h', 'apps/solidterm/native/terminalview.h', 'apps/solidterm/native/terminalpanes.h', 'apps/solidterm/native/terminaltabs.h', 'apps/solidterm/native/tmuxsession.h', 'apps/solidterm/native/systemtheme.h', 'apps/solidterm/native/keyrecorder.h', 'apps/solidterm/native/termconfig.h', 'apps/solidterm/native/paneheader.h'),
    qresources: files('apps/solidterm/shaders.qrc'),
  )
  solidterm_exe = executable(
    'solidterm',
    files(
      'apps/solidterm/main.cpp',
      'apps/solidterm/native/ptysession.cpp',
      'apps/solidterm/native/terminalview.cpp',
      'apps/solidterm/native/termscreen.cpp',
      'apps/solidterm/native/termworker.cpp',
      'apps/solidterm/native/glyphcache.cpp',
      'apps/solidterm/native/terminalpanes.cpp',
      'apps/solidterm/native/terminaltabs.cpp',
      'apps/solidterm/native/systemtheme.cpp',
      'apps/solidterm/native/keyrecorder.cpp',
      'apps/solidterm/native/termconfig.cpp',
      'apps/solidterm/native/paneheader.cpp',
      'apps/solidterm/native/singleinstance.cpp',
      'apps/solidterm/native/panedropselftest.cpp',
      'apps/solidterm/native/tmuxsession.cpp',
      'apps/solidterm/native/tmuxselftest.cpp',
      'apps/solidterm/native/tmux/protocol.cpp',
      'apps/solidterm/native/tmux/layout.cpp',
      'apps/solidterm/native/tmux/seed.cpp',
      'apps/solidterm/native/tmux/client.cpp',
    ),
    blur_src,
    solidterm_moc,
    cpp_args: notify_args + blur_args + ['-DSOLIDTERM_DEV_DIR="' + meson.current_source_dir() + '/apps/solidterm/qml"'],
    include_directories: include_directories('src', 'apps/solidterm'),
    # qt6_templates_private_dep (declared above) already pulls Gui+Quick private headers, which is
    # what glyphcache.cpp needs for <rhi/qrhi.h> (GlyphAtlasTexture's partial atlas upload, P3) —
    # reused here rather than declaring another private-headers dependency() for the same modules.
    dependencies: [solidqml_dep, qt6_widgets_dep, qt6_network_dep, vterm_dep, util_dep, qt6_templates_private_dep] + blur_deps, # Network: QLocalServer (single instance)
    install_rpath: '$ORIGIN/../' + get_option('libdir'),
    install: true,
  )

  # solidterm install: the generated UI (qml + css + component .qml) lands in share/solidterm,
  # which main resolves as <bindir>/../share/solidterm; plus the .desktop entry and icon so the
  # terminal shows up in the launcher (owner: "a ideia é ser instalável").
  install_subdir('apps/solidterm/qml', install_dir: get_option('datadir') / 'solidterm', strip_directory: true)
  install_data('apps/solidterm/solidterm.desktop', install_dir: get_option('datadir') / 'applications')
  install_data('apps/solidterm/solidterm.svg',
    install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
    rename: 'solidterm.svg')

  # libvterm scrollback round-trip (no Qt): a width-0 padding cell in sb_popline hung SolidTerm at
  # 100% CPU whenever the window grew wider AND taller (workspace move, resize). The bug is a hang,
  # so the short timeout is the assertion.
  test('solidterm-scrollback',
    executable('test-solidterm-scrollback',
      files('apps/solidterm/tests/scrollback_test.cpp', 'apps/solidterm/native/termscreen.cpp'),
      dependencies: [vterm_dep]),
    timeout: 10)

  # Text anchoring (lineanchor.h): selection / search matches / hover link stored as line numbers
  # (TermScreen::lineBase() + absolute row) follow their text through pushes below AND at the
  # scrollback limit, resizes, a lower limit and a clear, and read as evicted once the text is gone.
  test('solidterm-lineanchor',
    executable('test-solidterm-lineanchor',
      files('apps/solidterm/tests/lineanchor_test.cpp', 'apps/solidterm/native/termscreen.cpp'),
      dependencies: [vterm_dep]),
    timeout: 10)

  # TermScreen (SolidTerm's own screen buffer over libvterm's state layer) vs the previous
  # VTermScreen path, kept here as the reference model: an escape-sequence corpus + seeded random
  # streams + resizes, cells/cursor/line info/scrollback compared after every step. A hang in the
  # sb_popline round trip (width-0 padding) shows up as the timeout. See termscreen_oracle_test.cpp.
  test('solidterm-termscreen-oracle',
    executable('test-solidterm-termscreen-oracle',
      files('apps/solidterm/tests/termscreen_oracle_test.cpp', 'apps/solidterm/native/termscreen.cpp'),
      dependencies: [vterm_dep]),
    timeout: 120)

  # TermWorker, the per-pane parser thread (docs/solidterm-parser-thread.md), against a real pty and
  # child, no Qt: a 20 MB flood arrives intact with throttled GUI wakes, pause stops parsing and
  # resume delivers 1..N in order, queued output never blocks, stop() mid-flood joins promptly, the
  # child's exit is reported. See termworker_test.cpp.
  test('solidterm-termworker',
    executable('test-solidterm-termworker',
      files('apps/solidterm/tests/termworker_test.cpp', 'apps/solidterm/native/termworker.cpp'),
      dependencies: [util_dep, dependency('threads')]),
    timeout: 120)

  # tmux control-mode protocol library (docs/solidterm-tmux-control-mode.md, phase 0), no Qt, no tmux:
  # golden transcripts (hand-written from tmux 3.7c's source and one recorded from a real 3.7c, see
  # apps/solidterm/tests/tmux/record-transcript.sh) fed in every chunking, plus decoder / correlator /
  # quoting / layout unit checks. See tmux_protocol_test.cpp.
  solidterm_tmux_proto = files('apps/solidterm/native/tmux/protocol.cpp', 'apps/solidterm/native/tmux/layout.cpp')
  test('solidterm-tmux-protocol',
    executable('test-solidterm-tmux-protocol',
      files('apps/solidterm/tests/tmux_protocol_test.cpp') + solidterm_tmux_proto,
      cpp_args: ['-DTMUX_TRANSCRIPTS="' + meson.current_source_dir() / 'apps/solidterm/tests/tmux' + '"']),
    timeout: 20)

  # tmux control client (phase 1, Qt-free): the real reader thread and per-pane feed pipes against a
  # scripted FAKE tmux (tests/tmux/fake-tmux.py playing scenario-basic.py), each pipe parsed by a bare
  # TermScreen — seeding rules, tricky %output, held output of new panes, the resize fence, %pause →
  # reseed, detach. The second test attaches the same client to a REAL tmux on a private socket
  # (-S under the build dir, -f /dev/null; never the user's server), skipped when tmux is absent.
  solidterm_tmux_client = solidterm_tmux_proto + files('apps/solidterm/native/tmux/client.cpp', 'apps/solidterm/native/tmux/seed.cpp')
  tmux_client_test = executable('test-solidterm-tmux-client',
    files('apps/solidterm/tests/tmux_client_test.cpp', 'apps/solidterm/native/termscreen.cpp') + solidterm_tmux_client,
    cpp_args: ['-DTMUX_TESTS_DIR="' + meson.current_source_dir() / 'apps/solidterm/tests/tmux' + '"'],
    dependencies: [vterm_dep, dependency('threads')])
  test('solidterm-tmux-client', tmux_client_test,
    args: [meson.current_build_dir() / 'tmux-test'],
    timeout: 60)
  tmux_prog = find_program('tmux', required: false)
  test('solidterm-tmux-client-real', tmux_client_test,
    args: ['--real', tmux_prog.found() ? tmux_prog.full_path() : '/nonexistent/tmux', meson.current_build_dir() / 'tmux-real'],
    is_parallel: false,
    timeout: 60)

  # tmux key table (phase 2): Qt key events → send-keys key names / literals (native/tmux/keys.h).
  test('solidterm-tmux-keys',
    executable('test-solidterm-tmux-keys', files('apps/solidterm/tests/tmux_keys_test.cpp'),
      dependencies: [dependency('qt6', modules: ['Core'])]),
    timeout: 10)

  # Mouse reporting (xterm/VTE protocol): the decision table (mode/altscreen/shift →
  # report|altscroll|local) plus libvterm's own SGR/X10/DRAG/wheel/DECCKM encoding, driven the same
  # way TerminalView drives it. No Qt — see mousereport_test.cpp.
  test('solidterm-mousereport',
    executable('test-solidterm-mousereport', files('apps/solidterm/tests/mousereport_test.cpp'),
      dependencies: [vterm_dep]),
    timeout: 10)

  # Pane drag & drop across windows (the app itself, offscreen, self-checking): an unaccepted drag
  # detaches a pane to a new window; dropping it on another window's split zone moves the LIVE view
  # (same shell PID) and closes the emptied window; a tab-strip drop makes a new tab; a drop on its
  # own pane is a no-op. Drives the real drop path (window drag events → TerminalTabs routing →
  # TerminalPanes::finishDrag); only QDrag::exec itself (the compositor's half) is not exercised.
  test('solidterm-pane-drop', solidterm_exe,
    args: ['--standalone'],
    env: {
      'QT_QPA_PLATFORM': 'offscreen',
      'QT_FORCE_STDERR_LOGGING': '1',
      'SHELL': '/bin/sh',
      'XDG_CONFIG_HOME': meson.current_build_dir() / 'solidterm-test-config',
      'SOLIDTERM_XDROPTEST': '1',
      'SOLIDTERM_PIDLOG': meson.current_build_dir() / 'solidterm-xdrop-pid.log',
    },
    is_parallel: false,
    timeout: 40)
  # The selection sticks to its text (the app itself, offscreen, self-checking — SOLIDTERM_SELANCHORTEST
  # in main.cpp): with the scrollback limit full, a drag-selected word keeps its text and its
  # highlight row moves up with it as the shell prints more, follows a scroll back, and is cleared
  # once the text is evicted. Exit code is the verdict.
  test('solidterm-selanchor', solidterm_exe,
    args: ['--standalone'],
    env: {
      'QT_QPA_PLATFORM': 'offscreen',
      'QT_FORCE_STDERR_LOGGING': '1',
      'SHELL': '/bin/sh',
      'XDG_CONFIG_HOME': meson.current_build_dir() / 'solidterm-test-config',
      'SOLIDTERM_SELANCHORTEST': '1',
    },
    timeout: 40)
  # tmux mode through the real UI, offscreen, self-checking (SOLIDTERM_TMUXTEST, tmuxselftest.h):
  # `--tmux work` against the scripted FAKE tmux — tabs/splits from layouts, cell-exact geometry,
  # seeded and live TermScreen contents equal to an oracle, re-split, window add/rename/close,
  # %pause → reseed, read-only input, detach on window close (the fake logs what it received).
  test('solidterm-tmux-ui', solidterm_exe,
    args: ['--standalone', '--tmux', 'work'],
    env: {
      'QT_QPA_PLATFORM': 'offscreen',
      'QT_FORCE_STDERR_LOGGING': '1',
      'XDG_CONFIG_HOME': meson.current_build_dir() / 'solidterm-test-config',
      'SOLIDTERM_TMUXTEST': 'fake',
      'SOLIDTERM_TMUX_BIN': meson.current_source_dir() / 'apps/solidterm/tests/tmux/fake-tmux.py',
      'FAKE_TMUX_SCENARIO': meson.current_source_dir() / 'apps/solidterm/tests/tmux/scenario-basic.py',
      'FAKE_TMUX_LOG': meson.current_build_dir() / 'solidterm-tmux-ui-fake.log',
    },
    timeout: 60)
  # The same UI against a REAL tmux on a private socket (skipped without tmux).
  test('solidterm-tmux-ui-real', find_program('bash'),
    args: [meson.current_source_dir() / 'apps/solidterm/tests/tmux/run-real-ui.sh', solidterm_exe.full_path(),
           tmux_prog.found() ? tmux_prog.full_path() : '', meson.current_build_dir() / 'tmux-real-ui'],
    env: { 'XDG_CONFIG_HOME': meson.current_build_dir() / 'solidterm-test-config' },
    depends: solidterm_exe,
    is_parallel: false,
    timeout: 120)

  # tmux INPUT through the real UI against the fake tmux (phase 2, SOLIDTERM_TMUXTEST=input): the
  # exact send-keys command lines for a key corpus (names × modifiers, coalesced -l literals, quoting,
  # IME commit), a 100 KB bracketed paste in <= 1 KiB -H chunks, SGR mouse click + wheel, focus
  # reports, OSC 10/11 colours via refresh-client -r, and no answers to queries in the output.
  test('solidterm-tmux-input', solidterm_exe,
    args: ['--standalone', '--tmux', 'keys'],
    env: {
      'QT_QPA_PLATFORM': 'offscreen',
      'QT_FORCE_STDERR_LOGGING': '1',
      'XDG_CONFIG_HOME': meson.current_build_dir() / 'solidterm-test-config',
      'SOLIDTERM_TMUXTEST': 'input',
      'SOLIDTERM_TMUX_BIN': meson.current_source_dir() / 'apps/solidterm/tests/tmux/fake-tmux.py',
      'FAKE_TMUX_SCENARIO': meson.current_source_dir() / 'apps/solidterm/tests/tmux/scenario-input.py',
      'FAKE_TMUX_LOG': meson.current_build_dir() / 'solidterm-tmux-input-fake.log',
    },
    timeout: 60)
  # The same against a REAL tmux (private socket): typing runs in the shell, and a vim/less-style
  # program gets SGR clicks, wheel and a bracketed paste. Skipped without tmux.
  test('solidterm-tmux-input-real', find_program('bash'),
    args: [meson.current_source_dir() / 'apps/solidterm/tests/tmux/run-real-ui.sh', solidterm_exe.full_path(),
           tmux_prog.found() ? tmux_prog.full_path() : '', meson.current_build_dir() / 'tmux-real-ui', 'real-input'],
    env: { 'XDG_CONFIG_HOME': meson.current_build_dir() / 'solidterm-test-config' },
    depends: solidterm_exe,
    is_parallel: false,
    timeout: 120)
  # `solidterm --help` prints usage (with --tmux) and exits before any window or instance code.
  test('solidterm-help', find_program('sh'),
    args: ['-c', 'out=$(DISPLAY= WAYLAND_DISPLAY= QT_QPA_PLATFORM=none "$0" --help) && echo "$out" | grep -q -- "--tmux" && echo "$out" | grep -q "^Usage: solidterm"', solidterm_exe.full_path()],
    depends: solidterm_exe,
    timeout: 10)

  # Selection autoscroll (drag past the top/bottom edge): the pure edge-distance → lines/tick mapping
  # and the offset clamp, plus a simulated multi-tick walk proving the selection's moving end keeps
  # tracking the right absolute row as the view scrolls under a stationary pointer. No Qt, no vterm —
  # see autoscroll_test.cpp / autoscroll.h.
  test('solidterm-autoscroll',
    executable('test-solidterm-autoscroll', files('apps/solidterm/tests/autoscroll_test.cpp')),
    timeout: 10)

  # Scrollbar thumb geometry/paging (scrollbar.h) — the formulas the composed QML bar's JS mirrors
  # (TerminalView::composeScrollBar). No Qt, no vterm — see scrollbar_test.cpp.
  test('solidterm-scrollbar',
    executable('test-solidterm-scrollbar', files('apps/solidterm/tests/scrollbar_test.cpp')),
    timeout: 10)

  # URL-detection scanner (linkscan.h) used by TerminalView::linkAt — scheme/www. table, trailing
  # punctuation trimming, and a soft-wrapped row-join scenario. No Qt, no vterm — see linkscan_test.cpp.
  test('solidterm-linkscan',
    executable('test-solidterm-linkscan', files('apps/solidterm/tests/linkscan_test.cpp')),
    timeout: 10)

  # Output pause on a local selection drag (owner: "quando seleciono texto o stdout tem que
  # travar") — PtySession::pause()/resume() against a REAL pty + real child shell: pause() stops
  # OUR reads AND the child's own write(2) blocks on the full kernel pty buffer (backpressure, no
  # data lost); resume() drains it. No Qt GUI needed — PtySession is pure QtCore + POSIX, so this
  # links only Core+Test. See ptypause_test.cpp.
  ptypause_test_deps = [dependency('qt6', modules: ['Core', 'Test']), util_dep]
  test('solidterm-ptypause',
    executable('test-solidterm-ptypause',
      files('apps/solidterm/tests/ptypause_test.cpp', 'apps/solidterm/native/ptysession.cpp'),
      qt6.preprocess(
        moc_headers: files('apps/solidterm/native/ptysession.h'),
        moc_sources: files('apps/solidterm/tests/ptypause_test.cpp'),
        dependencies: ptypause_test_deps,
      ),
      dependencies: ptypause_test_deps),
    env: ['SHELL=/bin/sh'],
    timeout: 180) # generous: it waits for the kernel pty buffer to actually fill, which depends on
                  # how much CPU its child gets — slower when the whole suite runs in parallel

  # GlyphCache's dirty-rect tracking (P3, docs/evidence/solidterm/perf-2026-10-13.md finding 5):
  # a burst of new glyphs reports bytes proportional to the glyph cells rasterised, not the whole
  # 2048x2048 atlas, and cache hits / an idle frame report nothing. No scene graph or RHI needed —
  # GlyphCache itself is QImage + QPainter + QFont — but font loading needs a QGuiApplication, so
  # this links Core+Gui+Quick (glyphcache.h declares GlyphAtlasTexture, a QSGTexture) plus the
  # Gui/Quick private headers glyphcache.cpp needs for <rhi/qrhi.h>, via qt6_templates_private_dep.
  glyphcache_test_deps = [dependency('qt6', modules: ['Core', 'Gui', 'Quick']), qt6_templates_private_dep]
  test('solidterm-glyphcache-dirty',
    executable('test-solidterm-glyphcache-dirty',
      files('apps/solidterm/tests/glyphcache_dirty_test.cpp', 'apps/solidterm/native/glyphcache.cpp'),
      dependencies: glyphcache_test_deps),
    env: ['QT_QPA_PLATFORM=offscreen'],
    timeout: 20)
endif

# ── Tests ────────────────────────────────────────────────────────────────────────────────────
# The engine's C++ suites register above (subproject tests=enabled). Wire the transpiler's Node
# suite in too so `meson test` is the single honest entry point for BOTH halves. It runs the same
# `npm run test:transpiler` the docs use (a bare directory arg loses tsx over node --test's child
# processes → false green; the package.json glob is the form that actually reports all 495). Skipped
# cleanly where sh/npm are absent (the C++ engine tests still run).
sh_prog = find_program('sh', required: false)
if sh_prog.found()
  test(
    'transpiler',
    sh_prog,
    args: ['-c', 'npm run test:transpiler'],
    workdir: meson.project_source_root(),
    timeout: 180,
  )

  # Version-consistency gate (release-plan M8): meson.build's project() version is the single
  # source of truth; this fails the build if package.json / package-lock.json / either Sphinx
  # doc site's `release` drifts from it. See scripts/check-version-consistency.mjs.
  test(
    'version-consistency',
    sh_prog,
    args: ['-c', 'node scripts/check-version-consistency.mjs'],
    workdir: meson.project_source_root(),
    timeout: 30,
  )
endif

# C23 <For> row identity, app level (transpiled TSX run in the loader): reference identity by default,
# key="id" opt-in reuse — mount/cleanup counts and row-local state on an eager list (run.sh), and the
# rows entering the window of a virtualized one (run-virtual.sh).
if sh_prog.found() and is_variable('loader_exe')
  test('for-identity', sh_prog,
    args: [files('tests/c23/run.sh'), loader_exe, meson.current_build_dir() / 'c23-for-identity'],
    workdir: meson.project_source_root(), timeout: 120)
  test('for-identity-virtual', sh_prog,
    args: [files('tests/c23/run-virtual.sh'), loader_exe, meson.current_build_dir() / 'c23-for-virtual'],
    workdir: meson.project_source_root(), timeout: 300)
endif

# Accessibility of the CSS primitives (Text/Image): role/name semantics + the lazy cost contract
# (one bool per element until an AT client queries). Uses QtQuick private headers to observe it.
a11y_test_deps = [solidqml_dep, qt6_templates_private_dep, dependency('qt6', modules: ['Test'])]
test('widgets-a11y',
  executable('test-widgets-a11y', files('tests/a11y_test.cpp'),
    qt6.preprocess(moc_sources: files('tests/a11y_test.cpp'), dependencies: a11y_test_deps),
    include_directories: include_directories('src'),
    dependencies: a11y_test_deps),
  env: ['QT_QPA_PLATFORM=offscreen'], timeout: 60)

# Keyboard & accessibility for PLAIN elements (release-plan M5): tabIndex/role/aria-* transpiled
# onto a generic Div/Text — Tab order, Enter/Space activation, Accessible.role/name/state — using
# the SAME standard QtQuick facilities (activeFocusOnTab, Keys, the Accessible attached type) a
# native widget's own .qml already relies on. No engine change; see docs/keyboard-accessibility.md.
test('widgets-a11y-generic',
  executable('test-widgets-a11y-generic', files('tests/a11y_generic_test.cpp'),
    qt6.preprocess(moc_sources: files('tests/a11y_generic_test.cpp'), dependencies: a11y_test_deps),
    include_directories: include_directories('src'),
    dependencies: a11y_test_deps),
  env: ['QT_QPA_PLATFORM=offscreen'], timeout: 60)

# <select>'s SolidWidgets::Select: model/values/disabledFlags default empty + round-trip with
# NOTIFY, and `model: <reactive list>` keeps following a changing context property — the mechanism
# the dynamic <select> options plan (transpiler/src/emit/qml.ts emitSelect) depends on.
select_test_deps = [solidqml_dep, qt6_templates_private_dep, dependency('qt6', modules: ['Test'])]
test('select-widget',
  executable('test-select-widget', files('tests/select_test.cpp'),
    qt6.preprocess(moc_sources: files('tests/select_test.cpp'), dependencies: select_test_deps),
    include_directories: include_directories('src'),
    dependencies: select_test_deps),
  env: ['QT_QPA_PLATFORM=offscreen'], timeout: 60)

# <RichText>/<DocumentEditor> engine (RichTextHandler): modified tracking, the ODF load/save error
# contract, content/format state and an .odt round-trip — what an app driving the document uses.
if build_richtext
  richtext_test_deps = [solidqml_dep, qt6_templates_private_dep, dependency('qt6', modules: ['Core', 'Gui', 'Qml', 'Quick', 'Test'])] + core_private_deps
  test('richtext-layout',
    executable('test-richtext-layout', files('tests/richtext_layout_test.cpp'),
      qt6.preprocess(moc_sources: files('tests/richtext_layout_test.cpp'), dependencies: richtext_test_deps),
      include_directories: include_directories('src'), dependencies: richtext_test_deps),
    env: ['QT_QPA_PLATFORM=offscreen'], timeout: 120)
  test('layout-phase0',
    executable('test-layout-phase0', files('tests/layout_phase0_test.cpp'),
      qt6.preprocess(moc_sources: files('tests/layout_phase0_test.cpp'), dependencies: richtext_test_deps),
      cpp_args: richtext_args + ['-DPHASE0_SOURCE_DIR="' + meson.project_source_root() + '"'],
      include_directories: include_directories('src'),
      dependencies: richtext_test_deps),
    env: ['QT_QPA_PLATFORM=offscreen'], timeout: 120)
  test('richtext-handler',
    executable('test-richtext-handler', files('tests/richtext_test.cpp'),
      qt6.preprocess(moc_sources: files('tests/richtext_test.cpp'), dependencies: richtext_test_deps),
      cpp_args: richtext_args,
      include_directories: include_directories('src'),
      dependencies: richtext_test_deps),
    env: ['QT_QPA_PLATFORM=offscreen', 'RICHTEXT_BOOK=' + get_option('richtext_book')], timeout: 60)
endif

# <Swipe>: a click reaches the child; a sideways drag or touchpad swipe slides the row and acts.
test('widgets-swipe',
  executable('test-widgets-swipe', files('tests/swipe_test.cpp'),
    qt6.preprocess(moc_sources: files('tests/swipe_test.cpp'), dependencies: a11y_test_deps),
    include_directories: include_directories('src'),
    dependencies: a11y_test_deps),
  env: ['QT_QPA_PLATFORM=offscreen'], timeout: 60)
