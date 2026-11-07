#!/usr/bin/env bash
# Cut a numbered, installed release of ion (HANDOFF §15).
#
#   scripts/release.sh 0.1.0        # checkpoint release
#   scripts/release.sh 0.3.0-dev.4  # in-phase pre-release
#
# 1. Refuses a dirty tree or failing tests.
# 2. Sets the version in CMakeLists.txt and dates the CHANGELOG entry (one commit).
# 3. Builds Release (-O2, debug symbols split into ion.debug, vendored libs static).
# 4. Runs the tests and ion-bench against the Release build.
# 5. Tags vX.Y.Z (annotated; message = the CHANGELOG entry).
# 6. Installs to ~/ion/releases/vX.Y.Z/, points ~/.local/bin/ion at it, keeps old ones.
# 7. Writes ~/ion/releases/ion-vX.Y.Z-<arch>.tar.gz.
set -euo pipefail

die() { echo "release: $*" >&2; exit 1; }

[[ $# -eq 1 ]] || die "usage: scripts/release.sh <version>   e.g. 0.1.0 or 0.3.0-dev.4"
VERSION="$1"
[[ "$VERSION" =~ ^([0-9]+\.[0-9]+\.[0-9]+)(-dev\.[0-9]+)?$ ]] || die "version must look like 0.1.0 or 0.3.0-dev.4"
CORE="${BASH_REMATCH[1]}"
PRE="${BASH_REMATCH[2]}"
TAG="v$VERSION"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
RELEASES="$HOME/ion/releases"
DEST="$RELEASES/$TAG"
ARCH="$(uname -m)"
JOBS="$(nproc)"

# ---- 1. clean tree, tag free, tests pass -------------------------------------
[[ -z "$(git status --porcelain)" ]] || die "working tree is dirty; commit or stash first"
git rev-parse -q --verify "refs/tags/$TAG" >/dev/null && die "tag $TAG already exists"
[[ -e "$DEST" ]] && die "$DEST already exists"
grep -q "^## \[$VERSION\]\|^## \[Unreleased\]" CHANGELOG.md || die "CHANGELOG.md needs an [Unreleased] or [$VERSION] section"

echo "== tests (dev build)"
cmake -S . -B build -G Ninja >/dev/null
cmake --build build -j "$JOBS" >/dev/null
ctest --test-dir build --output-on-failure >/dev/null || die "tests fail on the current tree"

# ---- 2. version bump + changelog date -----------------------------------------
sed -i -E "s/^project\(ion VERSION [0-9.]+ /project(ion VERSION $CORE /" CMakeLists.txt
sed -i -E "s/^set\(ION_PRERELEASE \"[^\"]*\"\)/set(ION_PRERELEASE \"$PRE\")/" CMakeLists.txt
TODAY="$(date +%F)"
if ! grep -q "^## \[$VERSION\]" CHANGELOG.md; then
  sed -i "0,/^## \[Unreleased\]/s//## [Unreleased]\n\n## [$VERSION] - $TODAY/" CHANGELOG.md
fi
if [[ -n "$(git status --porcelain)" ]]; then
  git add CMakeLists.txt CHANGELOG.md
  git commit -q -m "Release $TAG"
fi

# ---- 3. Release build ----------------------------------------------------------
echo "== Release build"
rm -rf build-release
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-release -j "$JOBS" >/dev/null
BIN=build-release/ion
"$BIN" --version | grep -q -- "-dirty" && die "Release build reports a dirty tree"

# ---- 4. tests + bench against Release ------------------------------------------
echo "== tests (Release)"
ctest --test-dir build-release --output-on-failure >/dev/null || die "tests fail in the Release build (the version commit is kept; fix and re-run)"
echo "== ion-bench"
BENCH_OUT="$(build-release/ion-bench --seconds 3)"
echo "$BENCH_OUT"

# ---- 5. tag ---------------------------------------------------------------------
NOTES="$(awk -v v="$VERSION" '
  $0 ~ "^## \\[" v "\\]" {on=1; next}
  on && /^## \[/ {exit}
  on {print}' CHANGELOG.md)"
git tag -a "$TAG" -m "ion $VERSION" -m "$NOTES"

# ---- 6. install -------------------------------------------------------------------
echo "== install to $DEST"
mkdir -p "$DEST"
objcopy --only-keep-debug "$BIN" "$DEST/ion.debug"
objcopy --strip-debug --add-gnu-debuglink="$DEST/ion.debug" "$BIN" "$DEST/ion"
objcopy --strip-debug build-release/ion-bench "$DEST/ion-bench"
cp build-release/tests/fake-oxi "$DEST/fake-oxi"
cp CHANGELOG.md TESTING.md "$DEST/"
echo "$BENCH_OUT" > "$DEST/bench.txt"
if [[ -d demo ]]; then cp -r demo "$DEST/demo"; fi

mkdir -p "$HOME/.local/bin"
ln -sfn "$DEST/ion" "$HOME/.local/bin/ion"

# ---- 7. tarball ---------------------------------------------------------------------
TARBALL="$RELEASES/ion-$TAG-$ARCH.tar.gz"
tar -C "$RELEASES" -czf "$TARBALL" "$TAG"

echo
echo "released $("$DEST/ion" --version)"
echo "  installed  $DEST"
echo "  ion ->     $(readlink "$HOME/.local/bin/ion")"
echo "  tarball    $TARBALL"
echo "  push with: git push origin HEAD $TAG"
