#!/usr/bin/env bash
# Put the ptdata source for a given release tag into the build context.
#
#   scripts/fetch-ptdata-src.sh [<version>]      # default: see PTDATA_VERSION
#
# Containerfile.mdsip builds libptd3d from source, because the PUBLISHED conda
# package links libfdpio2 -- the sandbox needs -DPTDATA_WITH_FDPIO=OFF so the
# library is physically incapable of remote I/O.
#
# The source cannot simply be ADDed from a URL: GA-FDP/ptdata is an INTERNAL
# repository, so an anonymous fetch returns 404. Baking a token into the image
# build would work and is deliberately not done -- credentials in a build
# argument end up in the image history, and this is a sandbox whose whole
# premise is that a client already has code execution inside it.
#
# So the source arrives through the build context instead, produced here by
# whoever runs the build and already has access:
#
#   1. a sibling ptdata checkout (the developer case) -- `git archive`, offline
#   2. `gh api` (the CI case) -- uses the caller's existing credentials
#
# The tarball is gitignored. Re-run this when PTDATA_VERSION changes.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Keep in step with ARG PTDATA_VERSION in Containerfile.mdsip. Passed to the
# build as --build-arg so the two cannot silently disagree; see the README.
VERSION="${1:-${PTDATA_VERSION:-2.2.0}}"
TAG="release-${VERSION}"
OUT="$ROOT/ptdata-src.tar.gz"
CHECKOUT="${PTDATA_CHECKOUT:-$ROOT/../ptdata}"

if [ -d "$CHECKOUT/.git" ] && git -C "$CHECKOUT" rev-parse -q --verify "$TAG" >/dev/null; then
    echo "using $CHECKOUT at $TAG"
    # --prefix so the archive has a single top-level directory, which the
    # Containerfile strips. Without it the extract scatters into /tmp/ptdata.
    git -C "$CHECKOUT" archive --format=tar.gz --prefix=ptdata/ "$TAG" -o "$OUT"
elif command -v gh >/dev/null 2>&1; then
    echo "no local $TAG in $CHECKOUT; fetching via gh api"
    gh api "repos/GA-FDP/ptdata/tarball/$TAG" > "$OUT"
else
    echo "cannot obtain ptdata $TAG." >&2
    echo "  Either check out GA-FDP/ptdata next to this repo (or set" >&2
    echo "  PTDATA_CHECKOUT), or install the gh CLI and authenticate." >&2
    exit 1
fi

# stat, not du: du reports allocated blocks and on this filesystem prints 512
# for a 1.8 MB file, which reads exactly like a truncated download.
echo "wrote $OUT ($(stat -c%s "$OUT") bytes)"

# Fail here rather than three minutes into a container build: a truncated or
# HTML-error-page "tarball" is a perfectly valid file and only announces
# itself when tar runs.
tar -tzf "$OUT" >/dev/null
echo "verified: readable gzip tar"
