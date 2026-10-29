#!/usr/bin/env bash
# Assemble the release files for one platform: out/CoD2x_<version>_<platform>.zip (binary + its
# companion + the manual) plus the raw binary copied into out/ - both become GitHub Release assets.
# Run from the repo root in the release publish job (bash, ubuntu-latest). CI test builds upload the
# raw binary on its own and do not call this; the makefile builds locally but does not package.
#
# Usage: package.sh <windows|linux> <version>
set -eu

PLATFORM="${1:-}"; VERSION="${2:-}"
[ -n "$PLATFORM" ] && [ -n "$VERSION" ] || { echo "usage: $0 <windows|linux> <version>" >&2; exit 2; }

case "$PLATFORM" in
  windows) BINARY=mss32.dll;   EXTRA=bin/windows/mss32_original.dll ;;
  linux)   BINARY=libCoD2x.so; EXTRA=bin/linux/cod2_lnxded ;;
  *) echo "unknown platform '$PLATFORM' (expected windows|linux)" >&2; exit 2 ;;
esac
BINDIR="bin/$PLATFORM"
MANUAL="zip/$PLATFORM/CoD2x Installation and uninstallation manual.txt"

WS="$(pwd)"
mkdir -p "$WS/out"
cp "$BINDIR/$BINARY" "$WS/out/"                      # raw binary -> standalone release asset

# Zip the binary + its companion file + the manual, flattened (zip -j), via a temp stage so the
# archive holds only those files (no directory prefixes).
stage="$(mktemp -d)"
cp "$BINDIR/$BINARY" "$EXTRA" "$MANUAL" "$stage/"
( cd "$stage" && zip -j "$WS/out/CoD2x_${VERSION}_${PLATFORM}.zip" ./* )

echo "Packaged $PLATFORM $VERSION -> out/CoD2x_${VERSION}_${PLATFORM}.zip + $BINARY"
