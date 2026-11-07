#!/usr/bin/env bash
# Switch which installed release `ion` runs, for rollback or side-by-side comparison.
#
#   scripts/use-version.sh 0.1.0
#   scripts/use-version.sh          # list installed releases
set -euo pipefail

RELEASES="$HOME/ion/releases"
LINK="$HOME/.local/bin/ion"

if [[ $# -eq 0 ]]; then
  current="$(readlink "$LINK" 2>/dev/null || true)"
  for d in $(ls -1d "$RELEASES"/v* 2>/dev/null | sort -V); do
    mark=" "
    [[ "$current" == "$d/ion" ]] && mark="*"
    echo "$mark $(basename "$d")"
  done
  exit 0
fi

VERSION="${1#v}"
TARGET="$RELEASES/v$VERSION/ion"
[[ -x "$TARGET" ]] || { echo "use-version: v$VERSION isn't installed (run with no arguments to list)" >&2; exit 1; }
mkdir -p "$(dirname "$LINK")"
ln -sfn "$TARGET" "$LINK"
echo "ion -> $("$LINK" --version)"
