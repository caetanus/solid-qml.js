# Windows CI — the steps .github/workflows/windows.yml runs once the repo (with submodules) is
# checked out and the toolchain is on PATH: MSVC 2022 x64 developer environment, Qt 6.11.2
# msvc2022_64 (qmake/windeployqt on PATH, $env:QT_ROOT_DIR set), Node 26, meson + ninja.
# The Windows counterpart of scripts/ci-steps.sh (Linux); same step names where they overlap.
#
# Usage:
#   pwsh scripts/ci-windows.ps1                        # every step, in order
#   pwsh scripts/ci-windows.ps1 native-build native-test   # only the named steps
#
# Steps: install-qt versions npm-ci typecheck test-transpiler native-build native-test install deploy smoke package
#
# Env:
#   PREFIX_DIR  install prefix (default: <checkout>\.scratch\prefix)
#   SMOKE_DIR   where the "outside the checkout" tiny app is authored + run (default: a directory
#               NEXT TO the checkout, never inside it)
#   DIST_DIR    where the unsigned zip lands (default: <checkout>\.scratch\dist)
#   JOBS        ninja -j (default: number of processors)
#   QT_DIR      where install-qt unpacks Qt (default: a Qt directory NEXT TO the checkout)
#
# What differs from Linux (and why) is listed in docs/windows.md.
# Steps come as separate arguments (`ci-windows.ps1 a b c`), hence ValueFromRemainingArguments.
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Steps)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue' # Invoke-WebRequest's progress bar slows downloads a lot
# A native command's non-zero exit code throws (PowerShell 7.4+), like `set -e`.
$PSNativeCommandUseErrorActionPreference = $true

$AppDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$PrefixDir = if ($env:PREFIX_DIR) { $env:PREFIX_DIR } else { Join-Path $AppDir '.scratch\prefix' }
$SmokeDir = if ($env:SMOKE_DIR) { $env:SMOKE_DIR } else { Join-Path (Split-Path $AppDir -Parent) 'solidqml-ci-smoke' }
$DistDir = if ($env:DIST_DIR) { $env:DIST_DIR } else { Join-Path $AppDir '.scratch\dist' }
$Jobs = if ($env:JOBS) { $env:JOBS } else { $env:NUMBER_OF_PROCESSORS }
$BuildDir = Join-Path $AppDir 'build'
# Forward slashes: these paths are pasted into a TSX import and a meson option.
function Fwd([string]$p) { $p -replace '\\', '/' }

Set-Location $AppDir

# Qt 6.11.2 msvc2022_64 straight from the official online repository (download.qt.io), each archive
# checked against the repository's .sha1. aqtinstall 3.3.0 (the latest release) cannot install it:
# from 6.11 the repository is split per arch (qt6_6112/qt6_6112_msvc2022_64/), a layout it predates.
# Only the modules the build needs: qtbase (incl. private headers and windeployqt), qtdeclarative,
# qtsvg, plus the d3dcompiler/opengl32sw runtimes windeployqt bundles.
$QtVersion = '6.11.2'
$QtDir = if ($env:QT_DIR) { $env:QT_DIR } else { Join-Path (Split-Path $AppDir -Parent) 'Qt' }
$QtPrefix = Join-Path $QtDir "$QtVersion\msvc2022_64"
function Get-Text([string]$url) {
    $c = (Invoke-WebRequest -UseBasicParsing $url).Content
    if ($c -is [byte[]]) { [System.Text.Encoding]::UTF8.GetString($c) } else { $c }
}
function Step-InstallQt {
    Write-Host "== ci-windows: Qt $QtVersion msvc2022_64 -> $QtPrefix =="
    if (-not (Test-Path (Join-Path $QtPrefix 'bin\qmake.exe'))) {
        $tag = $QtVersion -replace '\.', ''
        $repo = "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_$tag/qt6_${tag}_msvc2022_64"
        $name = "qt.qt6.$tag.win64_msvc2022_64"
        [xml]$updates = Get-Text "$repo/Updates.xml"
        $pkg = $updates.Updates.PackageUpdate | Where-Object { $_.Name -eq $name }
        if (-not $pkg) { throw "ci-windows: $name not found in $repo/Updates.xml" }
        $dl = Join-Path $QtDir 'download'
        New-Item -ItemType Directory -Force $dl | Out-Null
        $wanted = $pkg.DownloadableArchives -split ',\s*' | Where-Object { $_ -match '^(qtbase|qtdeclarative|qtsvg|d3dcompiler_47|opengl32sw)' }
        foreach ($a in $wanted) {
            $url = "$repo/$name/$($pkg.Version)$a"
            $file = Join-Path $dl $a
            Write-Host "  $a"
            Invoke-WebRequest -UseBasicParsing $url -OutFile $file
            $sha = (Get-Text "$url.sha1").Trim().Split(' ')[0]
            $got = (Get-FileHash -Algorithm SHA1 $file).Hash
            if ($got -ne $sha) { throw "ci-windows: SHA1 mismatch for $a ($got, expected $sha)" }
            7z x -y -bso0 "-o$QtPrefix" $file # the archives hold the prefix contents (bin\, lib\...) directly
        }
        Remove-Item -Recurse -Force $dl
        if (-not (Test-Path (Join-Path $QtPrefix 'bin\qmake.exe'))) { Get-ChildItem $QtDir; throw "ci-windows: no qmake.exe under $QtPrefix after unpacking" }
    }
    # Later workflow steps see Qt like install-qt-action would expose it.
    if ($env:GITHUB_ENV) { "QT_ROOT_DIR=$QtPrefix" | Out-File -Append -Encoding utf8 $env:GITHUB_ENV }
    if ($env:GITHUB_PATH) { Join-Path $QtPrefix 'bin' | Out-File -Append -Encoding utf8 $env:GITHUB_PATH }
    & (Join-Path $QtPrefix 'bin\qmake.exe') -query QT_VERSION
}

function Step-Versions {
    Write-Host '== ci-windows: toolchain versions =='
    Write-Host "-- OS --"; [System.Environment]::OSVersion.VersionString
    Write-Host "-- MSVC --"; (Get-Command cl.exe).Source; $env:VSCMD_VER
    Write-Host "-- Qt --"; qmake -query QT_VERSION; qmake -query QT_INSTALL_PREFIX
    Write-Host "-- Node --"; node --version
    Write-Host "-- npm --"; npm.cmd --version
    Write-Host "-- meson --"; meson --version
    Write-Host "-- ninja --"; ninja --version
}

# npm.cmd, not the npm.ps1 shim.
function Step-NpmCi { Write-Host '== ci-windows: npm ci =='; npm.cmd ci }
function Step-Typecheck { Write-Host '== ci-windows: npm run typecheck =='; npm.cmd run typecheck }
function Step-TestTranspiler { Write-Host '== ci-windows: npm run test:transpiler =='; npm.cmd run test:transpiler }

function Step-NativeBuild {
    Write-Host "== ci-windows: meson setup + ninja -j $Jobs =="
    # cl, not a MinGW g++ that may also be on the runner's PATH.
    $env:CC = 'cl'; $env:CXX = 'cl'
    $setup = @('setup', $BuildDir, '--backend', 'ninja', '--buildtype', 'release', "--prefix=$(Fwd $PrefixDir)")
    if (Test-Path (Join-Path $BuildDir 'build.ninja')) { $setup += '--reconfigure' }
    meson @setup
    # -k 0: report every failing translation unit in one run, not just the first.
    ninja -C $BuildDir -j $Jobs -k 0
}

function Step-NativeTest {
    Write-Host '== ci-windows: meson test (offscreen) =='
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QT_FORCE_STDERR_LOGGING = '1'
    # The offscreen QPA on Windows has no native font database: it reads font files from
    # QT_QPA_FONTDIR (default <Qt>\lib\fonts, which Qt no longer ships). Without it every text test,
    # the CssTextLayout-vs-QQuickText oracle included, would measure glyph-less text.
    $env:QT_QPA_FONTDIR = Join-Path $env:SystemRoot 'Fonts'
    meson test -C $BuildDir --timeout-multiplier 3 --print-errorlogs
}

function Step-Install {
    Write-Host "== ci-windows: meson install (prefix $PrefixDir) =="
    if (Test-Path $PrefixDir) { Remove-Item -Recurse -Force $PrefixDir }
    meson install -C $BuildDir
}

# The tiny app OUTSIDE the checkout (tests/clean-room/hello-app), transpiled with the checkout's
# transpiler. Generated before `deploy` so windeployqt can scan its QML imports too.
function Initialize-SmokeApp {
    if (Test-Path $SmokeDir) { Remove-Item -Recurse -Force $SmokeDir }
    New-Item -ItemType Directory -Force $SmokeDir | Out-Null
    Copy-Item (Join-Path $AppDir 'tests\clean-room\hello-app\counter.css') $SmokeDir
    $runtime = Fwd (Join-Path $AppDir 'src\solid-qml\runtime')
    (Get-Content -Raw (Join-Path $AppDir 'tests\clean-room\hello-app\counter.tsx')).Replace('__RUNTIME__', $runtime) |
        Set-Content -NoNewline (Join-Path $SmokeDir 'counter.tsx')
    Push-Location $SmokeDir
    try {
        # Absolute file:// URL to tsx's loader: a bare `--import tsx` resolves from the invoking
        # file's directory, which is outside the checkout here (see scripts/ci-steps.sh).
        $tsx = ([System.Uri](Join-Path $AppDir 'node_modules\tsx\dist\loader.mjs')).AbsoluteUri
        node --import $tsx (Join-Path $AppDir 'scripts\gen.mjs') (Join-Path $SmokeDir 'counter.tsx') (Join-Path $SmokeDir 'qmlout\App.generated.qml')
    } finally { Pop-Location }
}

function Step-Deploy {
    Write-Host '== ci-windows: windeployqt the installed loader (Qt runtime for a clean machine) =='
    Initialize-SmokeApp
    $bin = Join-Path $PrefixDir 'bin'
    # The engine and the widgets also compose QML from C++ string literals (CssRect's render shell
    # imports QtQuick.Shapes/Effects, ...), which qmlimportscanner cannot see: collect those
    # `import Qt...` lines into a scan-only .qml so windeployqt ships those modules too.
    $scan = Join-Path $AppDir '.scratch\deploy-scan'
    New-Item -ItemType Directory -Force $scan | Out-Null
    $imports = Get-ChildItem -Recurse -Include *.cpp, *.h (Join-Path $AppDir 'src'), (Join-Path $AppDir 'vendor\qml-css-engine\src') |
        Select-String -Pattern 'import (Qt[A-Za-z0-9_.]*)' -AllMatches |
        ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
    Write-Host "QML imports composed from C++: $($imports -join ', ')"
    (($imports | ForEach-Object { "import $_" }) + 'Item {}') | Set-Content (Join-Path $scan 'CppComposedImports.qml')
    # --qmldir: every place QML the loader will load lives, so the QtQuick/Templates/Labs imports
    # they use are collected (the SDK's own base + widget QML, and a generated app).
    windeployqt --release --compiler-runtime --no-translations `
        --qmldir (Join-Path $AppDir 'src\solid-qml') `
        --qmldir (Join-Path $AppDir 'qml\solidqml\Widgets') `
        --qmldir (Join-Path $SmokeDir 'qmlout') `
        --qmldir $scan `
        (Join-Path $bin 'solid-qml-loader.exe')
    # The MSVC runtime DLLs themselves (windeployqt --compiler-runtime only adds the vc_redist
    # installer): the zip must run on a clean Windows without installing anything.
    $crt = Get-ChildItem -Directory (Join-Path $env:VCToolsRedistDir 'x64') -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
    if (-not $crt) { throw "ci-windows: no MSVC CRT redist under $env:VCToolsRedistDir" }
    Copy-Item (Join-Path $crt.FullName '*.dll') $bin
    # windeployqt ships only the windows QPA plugin; headless runs (CI, the smoke test) need offscreen.
    New-Item -ItemType Directory -Force (Join-Path $bin 'platforms') | Out-Null
    Copy-Item (Join-Path $QtPrefix 'plugins\platforms\qoffscreen.dll') (Join-Path $bin 'platforms')
}

function Step-Smoke {
    Write-Host "== ci-windows: installed-artifact smoke test — a NEW app OUTSIDE the checkout, at $SmokeDir =="
    if (-not (Test-Path (Join-Path $SmokeDir 'qmlout\App.generated.qml'))) { Initialize-SmokeApp }
    $loader = Join-Path $PrefixDir 'bin\solid-qml-loader.exe'
    $shot = Join-Path $SmokeDir 'shot.png'
    # A clean-machine PATH: only the deployed prefix + Windows itself — no Qt SDK, no MSVC, no Node.
    $saved = @{ PATH = $env:PATH; QT_PLUGIN_PATH = $env:QT_PLUGIN_PATH; QML2_IMPORT_PATH = $env:QML2_IMPORT_PATH; QML_IMPORT_PATH = $env:QML_IMPORT_PATH }
    $env:PATH = "$(Join-Path $PrefixDir 'bin');$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_PLUGIN_PATH = $null; $env:QML2_IMPORT_PATH = $null; $env:QML_IMPORT_PATH = $null
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QT_FORCE_STDERR_LOGGING = '1'
    $env:QT_QPA_FONTDIR = Join-Path $env:SystemRoot 'Fonts' # see Step-NativeTest (the windows QPA needs no such thing)
    Push-Location $SmokeDir
    try {
        $log = & $loader --qml (Join-Path $SmokeDir 'qmlout\App.generated.qml') --click 60,60,300 --grab $shot 2>&1 |
            ForEach-Object { "$_" }
        $log | Write-Host
        # A missing deployed QML module or font only logs; the screenshot alone would not show it.
        $bad = $log | Select-String -Pattern 'is not installed|Component is not ready|Cannot find font directory|ReferenceError|TypeError'
        if ($bad) { throw "ci-windows: the deployed loader logged errors:`n$($bad -join "`n")" }
    } finally {
        Pop-Location
        foreach ($k in $saved.Keys) { Set-Item "env:$k" $saved[$k] -ErrorAction SilentlyContinue }
    }
    if (-not (Test-Path $shot) -or (Get-Item $shot).Length -eq 0) { throw 'ci-windows: smoke test produced no screenshot' }
    New-Item -ItemType Directory -Force $DistDir | Out-Null
    Copy-Item $shot (Join-Path $DistDir 'smoke-shot.png')
    Write-Host "ci-windows: smoke test PASSED. Screenshot: $shot"
}

function Step-Package {
    Write-Host '== ci-windows: package the installed prefix (unsigned zip) =='
    New-Item -ItemType Directory -Force $DistDir | Out-Null
    $zip = Join-Path $DistDir 'solid-qml-windows-x64.zip'
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path (Join-Path $PrefixDir '*') -DestinationPath $zip
    Get-ChildItem $DistDir | Format-Table Name, Length
}

$All = 'install-qt', 'versions', 'npm-ci', 'typecheck', 'test-transpiler', 'native-build', 'native-test', 'install', 'deploy', 'smoke', 'package'
if (-not $Steps -or $Steps.Count -eq 0) { $Steps = $All }
foreach ($s in $Steps) {
    switch ($s) {
        'install-qt' { Step-InstallQt }
        'versions' { Step-Versions }
        'npm-ci' { Step-NpmCi }
        'typecheck' { Step-Typecheck }
        'test-transpiler' { Step-TestTranspiler }
        'native-build' { Step-NativeBuild }
        'native-test' { Step-NativeTest }
        'install' { Step-Install }
        'deploy' { Step-Deploy }
        'smoke' { Step-Smoke }
        'package' { Step-Package }
        default { throw "ci-windows: unknown step '$s' (known: $($All -join ' '))" }
    }
}
