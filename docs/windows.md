# Windows

solid-qml builds and runs its tests on Windows x64 with MSVC 2022. The
`.github/workflows/windows.yml` workflow does this on every push, using the steps in
`scripts/ci-windows.ps1`. That script is the Windows counterpart of `scripts/ci-steps.sh`.

## Toolchain

| Tool | Version | Where it comes from |
| --- | --- | --- |
| MSVC | 2022, x64, C++20 | the runner's Visual Studio (`ilammy/msvc-dev-cmd`) |
| Qt | 6.11.2 `msvc2022_64` | `ci-windows.ps1 install-qt` (see below) |
| Node | 26.8.2 | `actions/setup-node` |
| meson / ninja | 1.12.0 / 1.13.0 | `pip` |

**Why Qt is not installed with aqtinstall.** From Qt 6.11 on, the online repository has one
directory per architecture (`qt6_6112/qt6_6112_msvc2022_64/`). aqtinstall 3.3.0, the latest
release, does not know this layout and fails to find the version. `install-qt` therefore reads
that directory's `Updates.xml` itself. It downloads the archives for `qtbase` (which includes the
private headers and `windeployqt`), `qtdeclarative` and `qtsvg`, plus the `d3dcompiler_47` and
`opengl32sw` runtimes. It checks each archive against its `.sha1` file and unpacks it.

To build locally, open a "x64 Native Tools" prompt and put Qt's `bin` on `PATH`. Then run:

```powershell
pwsh scripts/ci-windows.ps1 install-qt npm-ci native-build native-test
```

## What differs from Linux

- **`libsolidqml` is a static library** (`libsolidqml.a`, next to the engine's
  `libqmlcssengine.a`). The code has no `dllexport`/`dllimport` annotations. The loader, the AOT
  demos and the C++ tests use internal classes, including their `staticMetaObject` data symbols,
  so a DLL would need an export macro on every class. On Windows the build defines
  `SOLIDQML_STATIC`. `SolidWidgets::registerTypes()` then calls `Q_INIT_RESOURCE` for the base
  stylesheet and the `solidqml.Widgets` QML, which a static link would otherwise drop. The CMake
  package (`find_package(SolidQml)`) imports the library as `STATIC` on Windows and links the
  engine and the Qt modules it uses.
- **SolidTerm is not built.** Its pty layer uses POSIX `forkpty()`, and there is no ConPTY port
  yet. meson prints a warning when it skips SolidTerm, and `-Dsolidterm=enabled` fails with an
  error that says why. The framework, loader, examples and tests are unaffected.
- **Desktop notifications are unavailable.** The backend is `org.freedesktop.Notifications` over
  D-Bus, and Windows has no session bus. meson prints a warning, and the `notifications` shim
  reports `available: false`. The tray icon is unaffected: it uses Qt Labs Platform's
  `SystemTrayIcon`, which works on Windows.
- **Wayland background blur** is part of SolidTerm, so it is not built either.
- **Headless runs need `QT_QPA_FONTDIR`.** On Windows the `offscreen` platform plugin has no
  native font database. It loads font files only from `QT_QPA_FONTDIR`, which defaults to
  `<Qt>\lib\fonts`, a directory Qt no longer ships. CI sets the variable to `%SystemRoot%\Fonts`
  for `meson test` and the smoke test. Without it, the text tests (including the
  CssTextLayout-vs-QQuickText oracle) would measure text with no glyphs. The normal `windows`
  platform plugin does not need this variable.
- **Line endings.** `.gitattributes` checks every text file out with LF. Git on Windows defaults
  to `core.autocrlf=true`, which would add CRLF to the transpiler goldens and fixtures that the
  tests compare byte for byte.

No test is skipped on Windows only. The tests skipped there are the ones also skipped on Linux CI:
the RichText 300-page-book tests (the sample file is not in the repository), the opt-in RichText
performance budget, and the opt-in repeater benchmark.

## The unsigned artifact

Each run uploads `solid-qml-windows-x64-unsigned`, which contains `solid-qml-windows-x64.zip`, the
installed prefix:

- `bin\solid-qml-loader.exe`, plus the Qt runtime collected by `windeployqt`: the Qt DLLs,
  `platforms\` (`qwindows`, and `qoffscreen` for headless runs), and `qml\` with QtQuick,
  Templates, Shapes, Effects, Window and Qt.labs.platform. It also contains the MSVC runtime
  DLLs and `vc_redist.x64.exe`.
- `include\solidqml\solidqmlembed.h` and `include\qmlcss\*.h`.
- `lib\libsolidqml.a` and `lib\libqmlcssengine.a`.
- `lib\pkgconfig\{solidqml,qml-css-engine}.pc` and `lib\cmake\SolidQml\`.

`windeployqt` cannot see QML that the engine builds from C++ strings (for example, CssRect's
render shell imports QtQuick.Shapes and QtQuick.Effects). The deploy step therefore collects those
`import Qt...` lines into a `.qml` file used only for scanning.

The smoke test writes a tiny app outside the checkout (`tests/clean-room/hello-app`), transpiles
it, and runs it with the deployed loader. The run uses a `PATH` that contains only the prefix and
`%SystemRoot%`: no Qt SDK, MSVC or Node. The test fails if the loader logs a missing QML module, a
missing font directory or a JavaScript error. Its screenshot is uploaded as
`windows-smoke-test-screenshot`.

The opt-in viz and media modules (`Chart`, `Scene3D`, `Surface`, `Media`, `Web`) need Qt add-ons
that are not in the zip (QtGraphs, QtQuick3D, QtMultimedia, QtWebEngine).
