#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PANEL_W="$(cygpath -w "$ROOT/panel")"
BUILD_W="$(cygpath -w "$ROOT/panel/build")"
QT_W="$(cygpath -w "$ROOT/runtime/Qt/6.8.3/msvc2022_64")"
cat > /tmp/build_panel.bat <<BAT
@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake -S "$PANEL_W" -B "$BUILD_W" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_W"
if errorlevel 1 exit /b 1
cmake --build "$BUILD_W"
BAT
cmd //c "$(cygpath -w /tmp/build_panel.bat)"
