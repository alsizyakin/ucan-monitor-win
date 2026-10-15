#!/bin/sh
# Prepares upstream/ for Windows: writes the files listed in renames under
# their new names, then applies win/patches/*.patch. A patch already
# applied, or taken into upstream, is skipped; one that no longer applies
# stops the build. Run it after win/checkout-upstream.sh.
set -e

cd "$(dirname "$0")/../../upstream"

grep -v '^#' ../win/patches/renames | awk 'NF { print $1, $2 }' |
while read -r path newpath; do
    if ! git show "HEAD:$path" > "$newpath"; then
        echo "$path: not in upstream any more, update win/patches/renames" >&2
        exit 1
    fi
    echo "$path: written as $newpath"
done

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
