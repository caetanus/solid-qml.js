# Windows CI — the steps .github/workflows/windows.yml runs once the repo (with submodules) is
# checked out and the toolchain is on PATH: MSVC 2022 x64 developer environment, Qt 6.11.2
# msvc2022_64 (qmake/windeployqt on PATH, $env:QT_ROOT_DIR set), Node 26, meson + ninja.
# The Windows counterpart of scripts/ci-steps.sh (Linux); same step names where they overlap.
#
# Usage:
#   pwsh scripts/ci-windows.ps1                        # every step, in order
#   pwsh scripts/ci-windows.ps1 native-build native-test   # only the named steps
#
# Steps: versions npm-ci typecheck test-transpiler native-build native-test install deploy smoke package
#
# Env:
#   PREFIX_DIR  install prefix (default: <checkout>\.scratch\prefix)
#   SMOKE_DIR   where the "outside the checkout" tiny app is authored + run (default: a directory
#               NEXT TO the checkout, never inside it)
#   DIST_DIR    where the unsigned zip lands (default: <checkout>\.scratch\dist)
#   JOBS        ninja -j (default: number of processors)
#
# What differs from Linux (and why) is listed in docs/windows.md.
param([string[]]$Steps)

$ErrorActionPreference = 'Stop'
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

function Step-Versions {
    Write-Host '== ci-windows: toolchain versions =='
    Write-Host "-- OS --"; [System.Environment]::OSVersion.VersionString
    Write-Host "-- MSVC --"; (Get-Command cl.exe).Source; $env:VSCMD_VER
    Write-Host "-- Qt --"; qmake -query QT_VERSION; qmake -query QT_INSTALL_PREFIX
    Write-Host "-- Node --"; node --version
    Write-Host "-- npm --"; npm --version
    Write-Host "-- meson --"; meson --version
    Write-Host "-- ninja --"; ninja --version
}

function Step-NpmCi { Write-Host '== ci-windows: npm ci =='; npm ci }
function Step-Typecheck { Write-Host '== ci-windows: npm run typecheck =='; npm run typecheck }
function Step-TestTranspiler { Write-Host '== ci-windows: npm run test:transpiler =='; npm run test:transpiler }

function Step-NativeBuild {
    Write-Host "== ci-windows: meson setup + ninja -j $Jobs =="
    # cl, not a MinGW g++ that may also be on the runner's PATH.
    $env:CC = 'cl'; $env:CXX = 'cl'
    $setup = @('setup', $BuildDir, '--backend', 'ninja', '--buildtype', 'release', "--prefix=$(Fwd $PrefixDir)")
    if (Test-Path (Join-Path $BuildDir 'build.ninja')) { $setup += '--reconfigure' }
    meson @setup
    ninja -C $BuildDir -j $Jobs
}

function Step-NativeTest {
    Write-Host '== ci-windows: meson test (offscreen) =='
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QT_FORCE_STDERR_LOGGING = '1'
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
    # --qmldir: every place QML the loader will load lives, so the QtQuick/Templates/Labs imports
    # they use are collected (the SDK's own base + widget QML, and a generated app).
    windeployqt --release --compiler-runtime --no-translations `
        --qmldir (Join-Path $AppDir 'src\solid-qml') `
        --qmldir (Join-Path $AppDir 'qml\solidqml\Widgets') `
        --qmldir (Join-Path $SmokeDir 'qmlout') `
        (Join-Path $bin 'solid-qml-loader.exe')
    # windeployqt ships only the windows QPA plugin; headless runs (CI, the smoke test) need offscreen.
    New-Item -ItemType Directory -Force (Join-Path $bin 'platforms') | Out-Null
    Copy-Item (Join-Path $env:QT_ROOT_DIR 'plugins\platforms\qoffscreen.dll') (Join-Path $bin 'platforms')
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
    Push-Location $SmokeDir
    try {
        & $loader --qml (Join-Path $SmokeDir 'qmlout\App.generated.qml') --click 60,60,300 --grab $shot
    } finally {
        Pop-Location
        foreach ($k in $saved.Keys) { Set-Item "env:$k" $saved[$k] -ErrorAction SilentlyContinue }
    }
    if (-not (Test-Path $shot) -or (Get-Item $shot).Length -eq 0) { throw 'ci-windows: smoke test produced no screenshot' }
    Write-Host "ci-windows: smoke test PASSED. Screenshot: $shot"
}

function Step-Package {
    Write-Host '== ci-windows: package the installed prefix (unsigned zip) =='
    New-Item -ItemType Directory -Force $DistDir | Out-Null
    $zip = Join-Path $DistDir 'solid-qml-windows-x64.zip'
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path (Join-Path $PrefixDir '*') -DestinationPath $zip
    $shot = Join-Path $SmokeDir 'shot.png'
    if (Test-Path $shot) { Copy-Item $shot (Join-Path $DistDir 'smoke-shot.png') }
    Get-ChildItem $DistDir | Format-Table Name, Length
}

$All = 'versions', 'npm-ci', 'typecheck', 'test-transpiler', 'native-build', 'native-test', 'install', 'deploy', 'smoke', 'package'
if (-not $Steps -or $Steps.Count -eq 0) { $Steps = $All }
foreach ($s in $Steps) {
    switch ($s) {
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
