#!/bin/sh
# Applies win/patches/*.patch to upstream/: fixes Windows needs until
# upstream has them. A patch already applied, or taken into upstream, is
# skipped; one that no longer applies stops the build.
set -e

cd "$(dirname "$0")/../../upstream"

for patch in ../win/patches/*.patch; do
    name=$(basename "$patch")
    if git apply --reverse --check "$patch" 2>/dev/null; then
        echo "$name: already in upstream, skipped"
    elif git apply --check "$patch"; then
        git apply "$patch"
        echo "$name: applied"
    else
        echo "$name: no longer applies, upstream changed these lines" >&2
        exit 1
    fi
done
