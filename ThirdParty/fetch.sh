#!/usr/bin/env bash
# Reproduces the vendored third-party sources listed in manifest.toml.
set -euo pipefail

root="$(cd "$(dirname "$0")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Clones `url` at `tag` and fails unless it resolves to the pinned `commit`.
checkout() {
    local name="$1" url="$2" tag="$3" commit="$4"
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$tag" "$url" "$work/$name"
    local actual
    actual="$(git -C "$work/$name" rev-parse HEAD)"
    if [[ "$actual" != "$commit" ]]; then
        echo "error: $name $tag resolved to $actual, expected $commit" >&2
        exit 1
    fi
}

checkout meshoptimizer https://github.com/zeux/meshoptimizer.git v1.3 \
    9e1f07b159d3cb777f1c67ed31fc11fd117986f4
rm -rf "$root/meshoptimizer"
mkdir -p "$root/meshoptimizer"
cp -R "$work/meshoptimizer/src" "$root/meshoptimizer/src"
cp "$work/meshoptimizer/LICENSE.md" "$root/meshoptimizer/LICENSE.md"
cp "$work/meshoptimizer/demo/clusterlod.h" "$root/meshoptimizer/src/clusterlod.h"

checkout doctest https://github.com/doctest/doctest.git v2.4.12 \
    1da23a3e8119ec5cce4f9388e91b065e20bf06f5
rm -rf "$root/doctest"
mkdir -p "$root/doctest/doctest"
cp "$work/doctest/doctest/doctest.h" "$root/doctest/doctest/doctest.h"
cp "$work/doctest/LICENSE.txt" "$root/doctest/LICENSE.txt"

checkout xxhash https://github.com/Cyan4973/xxHash.git v0.8.3 \
    e626a72bc2321cd320e953a0ccf1584cad60f363
rm -rf "$root/xxhash"
mkdir -p "$root/xxhash"
cp "$work/xxhash/xxhash.h" "$root/xxhash/xxhash.h"
cp "$work/xxhash/LICENSE" "$root/xxhash/LICENSE"

# Fonts come from the pinned commit directly, so the whole Plex repository is not cloned.
plex="https://raw.githubusercontent.com/IBM/plex/2f9ba1b25957d958db71a849e85d72e3ecfb845a"
rm -rf "$root/ibm-plex-mono"
mkdir -p "$root/ibm-plex-mono"
for weight in Regular Medium SemiBold Bold; do
    curl --fail --silent --show-error --output "$root/ibm-plex-mono/IBMPlexMono-$weight.ttf" \
        "$plex/packages/plex-mono/fonts/complete/ttf/IBMPlexMono-$weight.ttf"
done
curl --fail --silent --show-error --output "$root/ibm-plex-mono/LICENSE.txt" "$plex/LICENSE.txt"

echo "Vendored sources match manifest.toml."
