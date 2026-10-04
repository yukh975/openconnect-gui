#!/bin/bash
# Build a self-contained OpenConnect-GUI.app on macOS with Homebrew packages:
#   brew install cmake pkgconf qtbase qtscxml spdlog openconnect
# Usage: contrib/build_macos.sh [build-dir]   (default: ./build)
set -euo pipefail

SRC=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$SRC/build}
BREW=$(brew --prefix)

# Homebrew bottles are built for the running macOS release, so the bundle
# cannot run on anything older anyway; the project default (10.12) only
# produces libc++ "platform no longer supported" warnings.
MACOS_TARGET=${MACOS_TARGET:-$(sw_vers -productVersion | cut -d. -f1).0}

cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$BREW" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_TARGET"
cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu)"

APP=$BUILD/dist/OpenConnect-GUI.app
rm -rf "$BUILD/dist"
mkdir -p "$BUILD/dist"
cp -R "$BUILD/bin/OpenConnect-GUI.app" "$APP"

# macdeployqt exits non-zero when it cannot resolve QtSvg (see below); the
# result is verified explicitly at the end instead.
"$BREW/opt/qtbase/bin/macdeployqt" "$APP" -no-codesign || true

# The svg icon engine plugin needs QtSvg, which lives in its own Homebrew
# keg and is not picked up by macdeployqt.
if [ -e "$APP/Contents/PlugIns/iconengines/libqsvgicon.dylib" ] &&
   [ ! -e "$APP/Contents/Frameworks/QtSvg.framework" ]; then
    cp -R "$BREW/opt/qtsvg/lib/QtSvg.framework" "$APP/Contents/Frameworks/"
fi
chmod -R u+w "$APP"

# Mach-O files of the bundle, one per line.
macho_files() {
    find "$APP" -type f -print0 | while IFS= read -r -d '' f; do
        case $(file -b "$f") in (Mach-O*) printf '%s\n' "$f" ;; esac
    done
}
# Libraries a file links against (without its own install name).
deps() { otool -L "$1" | awk 'NR > 1 {print $1}'; }
# Its LC_RPATH entries.
rpaths() { otool -l "$1" | awk '$1 == "cmd" {r = ($2 == "LC_RPATH")} r && $1 == "path" {print $2}'; }

# Rewrite every remaining reference to Homebrew (install names of the copied
# libraries, their dependencies and rpaths) to point inside the bundle.
# grep is avoided in pipelines: with pipefail "no match" would abort the script.
macho_files | while IFS= read -r f; do
    id=$(otool -D "$f" | awk 'NR == 2')
    case "$id" in
    ("$BREW"/*) install_name_tool -id "@executable_path/../Frameworks/${id##*/lib/}" "$f" 2>/dev/null ;;
    esac
    deps "$f" | while IFS= read -r dep; do
        case "$dep" in
        ("$BREW"/*) install_name_tool -change "$dep" "@executable_path/../Frameworks/${dep##*/lib/}" "$f" 2>/dev/null ;;
        esac
    done
    rpaths "$f" | while IFS= read -r rp; do
        case "$rp" in
        ("$BREW"/*) install_name_tool -delete_rpath "$rp" "$f" ;;
        esac
    done
done

problems=$(macho_files | while IFS= read -r f; do
    deps "$f" | while IFS= read -r dep; do
        case "$dep" in
        ("$BREW"/*) echo "$f -> $dep" ;;
        (@executable_path/*) [ -e "$APP/Contents/MacOS/${dep#@executable_path/}" ] || echo "$f -> $dep (missing)" ;;
        esac
    done
    rpaths "$f" | while IFS= read -r rp; do
        case "$rp" in ("$BREW"/*) echo "$f rpath $rp" ;; esac
    done
done)
if [ -n "$problems" ]; then
    echo "The bundle is not self-contained:" >&2
    echo "$problems" >&2
    exit 1
fi

codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"
echo "Built $APP"
