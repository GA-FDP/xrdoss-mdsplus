#!/usr/bin/env bash
# Build the conda packages.
#
#   ./build_package.sh [output-dir]        # both packages, both variants
#
# The version comes from git, versioneer-style, so a package is always traceable
# to a commit:
#
#   on a release- tag        0.1.0
#   3 commits past one       0.1.0+3.g1a2b3c4
#   with uncommitted changes 0.1.0+3.g1a2b3c4.dirty
#   no tags at all           0.0.0+g1a2b3c4
#
# Everything else -- which packages, which variants -- is in recipe/.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

git rev-parse --git-dir >/dev/null 2>&1 \
  || { echo "not a git checkout: the version comes from git describe" >&2; exit 1; }

# --dirty is deliberate. A package built from a modified tree is not the tag it
# claims, and finding that out from the version string beats finding it out from
# behaviour.
if describe="$(git describe --tags --long --dirty --match 'release-*' 2>/dev/null)"; then
    # release-0.1.0-3-g1a2b3c4[-dirty]
    dirty=""
    case "$describe" in *-dirty) dirty=".dirty"; describe="${describe%-dirty}";; esac
    sha="${describe##*-g}"
    rest="${describe%-g*}"
    distance="${rest##*-}"
    tag="${rest%-*}"
    tag="${tag#release-}"
    if [ "$distance" = 0 ] && [ -z "$dirty" ]; then
        PKG_VERSION="$tag"
    else
        PKG_VERSION="${tag}+${distance}.g${sha}${dirty}"
    fi
else
    # No release- tag yet. Still produce something installable and traceable
    # rather than failing, but make it obvious it predates any release.
    sha="$(git rev-parse --short HEAD)"
    git diff --quiet HEAD 2>/dev/null || sha="${sha}.dirty"
    PKG_VERSION="0.0.0+g${sha}"
    echo "warning: no release-* tag found; version is ${PKG_VERSION}" >&2
fi
export PKG_VERSION

OUTPUT_DIR="${1:-${HOME}/outdir}"
mkdir -p "$OUTPUT_DIR"

echo "building version ${PKG_VERSION} -> ${OUTPUT_DIR}"

rattler-build build --recipe recipe/ \
  --channel ga-fdp \
  --channel conda-forge \
  --output-dir "$OUTPUT_DIR"
