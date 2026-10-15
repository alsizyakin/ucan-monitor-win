#!/bin/sh
# Checks out upstream/ and its own submodules, leaving out the files listed in
# win/patches/renames, which Windows 10 cannot hold. Use it instead of
# `git submodule update`; it works the same on Windows 10, 11 and Linux.
#
#   sh win/checkout-upstream.sh           the upstream commit recorded here
#   sh win/checkout-upstream.sh --latest  the tip of the upstream branch
set -e

cd "$(dirname "$0")/.."

url=$(git config -f .gitmodules submodule.upstream.url)
branch=$(git config -f .gitmodules submodule.upstream.branch)

fresh=
if [ ! -e upstream/.git ]; then
    rm -rf upstream
    git clone -q --no-checkout "$url" upstream
    fresh=1
fi

# The left-out files stay in the index, where Git for Windows would refuse
# them by name, but are never written to the disk. The patterns go in before
# any checkout: `git sparse-checkout set` may fill the working tree first
# without them, which fails on Windows 10.
git -C upstream config core.protectNTFS false
git -C upstream config core.sparseCheckout true
git -C upstream config core.sparseCheckoutCone false
(
    cd upstream
    sparse_file=$(git rev-parse --git-path info/sparse-checkout)
    mkdir -p "$(dirname "$sparse_file")"
    echo '/*' > "$sparse_file"
    grep -v '^#' ../win/patches/renames | awk 'NF { print "!/" $1 }' >> "$sparse_file"
)
# a tree checked out before gets the patterns applied now
[ -n "$fresh" ] || git -C upstream sparse-checkout reapply

if [ "$1" = --latest ]; then
    git -C upstream fetch -q origin "$branch"
    target=$(git -C upstream rev-parse FETCH_HEAD)
else
    target=$(git ls-tree HEAD upstream | awk '{ print $3 }')
    git -C upstream cat-file -e "$target^{commit}" 2>/dev/null \
        || git -C upstream fetch -q origin
fi

git -C upstream checkout -q --detach "$target"
git -C upstream submodule update -q --init --recursive
echo "upstream: $(git -C upstream log -1 --format='%h %s')"
