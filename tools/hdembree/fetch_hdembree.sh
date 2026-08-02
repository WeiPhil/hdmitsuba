#!/usr/bin/env bash
# Fetches the hdEmbree plugin sources from the OpenUSD repository, matching
# the locally installed USD version (override with: ./fetch_hdembree.sh vXX.YY).
#
# hdEmbree is USD's reference path-tracing render delegate. Prebuilt USD
# distributions (conda-forge, PyPI) omit it, which leaves the
# HdEmbree-comparison tests in this repo skipped. This kit builds just the
# plugin out of tree against the installed USD SDK; see README.md.
set -euo pipefail
cd "$(dirname "$0")"

TAG="${1:-}"
if [[ -z "${TAG}" ]]; then
  TAG=$(python -c "from pxr import Usd; v = Usd.GetVersion(); print(f'v{v[1]}.{v[2]:02d}')")
fi
echo "Fetching hdEmbree sources for OpenUSD ${TAG}"

if [[ -d src ]]; then
  echo "src/ already exists — delete it first to re-fetch." >&2
  exit 1
fi

CLONE_DIR=$(mktemp -d)
trap 'rm -rf "${CLONE_DIR}"' EXIT

git clone --quiet --depth 1 --branch "${TAG}" --filter=blob:none --sparse \
  https://github.com/PixarAnimationStudios/OpenUSD.git "${CLONE_DIR}/OpenUSD"
git -C "${CLONE_DIR}/OpenUSD" sparse-checkout set pxr/imaging/plugin/hdEmbree

cp -R "${CLONE_DIR}/OpenUSD/pxr/imaging/plugin/hdEmbree" src
rm -rf src/testenv
echo "Fetched $(find src -name '*.cpp' | wc -l | tr -d ' ') sources into src/. Next:"
echo '  cmake -B build -G Ninja -DCMAKE_PREFIX_PATH="$CONDA_PREFIX" -DCMAKE_INSTALL_PREFIX="$CONDA_PREFIX"'
echo '  cmake --build build && cmake --install build'
