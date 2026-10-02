#!/usr/bin/env bash
# release-notes.sh TAG [CHANGELOG]
#
# Checks a release tag against CHANGELOG.md and prints the matching section
# (without its heading) on stdout, for use as the release notes.
#
# Rule: TAG must be vX.Y.Z or vX.Y.Z-PRE (PRE = rc1, beta2, ...). The FIRST
# "## [" heading in the changelog must be "## [X.Y.Z]" - the base version, so
# v0.1.0-rc1, v0.1.0-rc2 and v0.1.0 all release from the same "## [0.1.0]"
# section. A final tag (no -PRE) additionally requires that the heading is
# not marked "Unreleased" (it must carry a date).

set -euo pipefail

TAG=${1:?Usage: $0 TAG [CHANGELOG]}
FILE=${2:-$(dirname "$0")/../CHANGELOG.md}

re='^v([0-9]+\.[0-9]+\.[0-9]+)(-[0-9A-Za-z.]+)?$'
[[ $TAG =~ $re ]] || { echo "Tag '$TAG' is not vX.Y.Z or vX.Y.Z-PRE." >&2; exit 1; }
BASE=${BASH_REMATCH[1]}
PRE=${BASH_REMATCH[2]}

[[ -f $FILE ]] || { echo "$FILE not found." >&2; exit 1; }
HEAD=$(grep -m1 '^## \[' "$FILE" || true)
[[ -n $HEAD ]] || { echo "No '## [version]' heading in $FILE." >&2; exit 1; }
if [[ $HEAD != "## [$BASE]"* ]]; then
    echo "Tag $TAG needs the top CHANGELOG heading to be '## [$BASE]', found: $HEAD" >&2
    exit 1
fi
if [[ -z $PRE && $HEAD == *[Uu]nreleased* ]]; then
    echo "Final release $TAG but the CHANGELOG heading is still marked Unreleased: $HEAD" >&2
    exit 1
fi

# Section body: from the line after the first heading to the next "## [".
NOTES=$(awk 'f && /^## \[/ {exit} f {print} /^## \[/ && !f {f=1}' "$FILE")
[[ -n ${NOTES//[[:space:]]/} ]] || { echo "CHANGELOG section for $BASE is empty." >&2; exit 1; }
printf '%s\n' "$NOTES"
