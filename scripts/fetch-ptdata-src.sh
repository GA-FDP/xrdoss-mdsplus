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
#   1. a local ptdata checkout (the developer case) -- `git archive`, offline
#   2. `git clone` over SSH (the deploy case) -- the SAME credential path that
#      already clones this repo, so a host that can build can always fetch
#   3. `gh api` (the CI case), where a token exists but no SSH key does
#
# The tarball is gitignored. Re-run this when PTDATA_VERSION changes.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Keep in step with ARG PTDATA_VERSION in Containerfile.mdsip and
# Containerfile.build. Passed to the
# build as --build-arg so the two cannot silently disagree; see the README.
VERSION="${1:-${PTDATA_VERSION:-2.3.0}}"
TAG="release-${VERSION}"
OUT="$ROOT/ptdata-src.tar.gz"
CHECKOUT="${PTDATA_CHECKOUT:-$ROOT/../ptdata}"
PTDATA_REPO="${PTDATA_REPO:-git@github.com:GA-FDP/ptdata.git}"

# --prefix so the archive has a single top-level directory, which the
# Containerfile strips. Without it the extract scatters into /tmp/ptdata.
archive_from() { git -C "$1" archive --format=tar.gz --prefix=ptdata/ "$TAG" -o "$OUT"; }

tried=""

if [ -d "$CHECKOUT/.git" ] && git -C "$CHECKOUT" rev-parse -q --verify "$TAG" >/dev/null; then
    echo "using $CHECKOUT at $TAG"
    archive_from "$CHECKOUT"
else
    tried="$tried\n  - local checkout $CHECKOUT (no .git, or no $TAG in it)"

    # git over SSH, before gh. build.sh on the deploy host already clones
    # xrdoss-mdsplus this way, so if the build can run at all this works --
    # whereas gh is frequently not installed there. Getting that precedence
    # backwards is what made the first deploy attempt fail.
    TMPC="$(mktemp -d "${TMPDIR:-/tmp}/ptdata-src.XXXXXX")"
    trap 'rm -rf "$TMPC"' EXIT
    if git clone --quiet --depth 1 --branch "$TAG" "$PTDATA_REPO" "$TMPC/ptdata" 2>/dev/null; then
        echo "cloned $PTDATA_REPO at $TAG"
        archive_from "$TMPC/ptdata"
    else
        tried="$tried\n  - git clone $PTDATA_REPO @ $TAG (no access, or no such tag)"
        if command -v gh >/dev/null 2>&1 && gh api "repos/GA-FDP/ptdata/tarball/$TAG" > "$OUT" 2>/dev/null; then
            echo "fetched via gh api"
        else
            tried="$tried\n  - gh api ($(command -v gh >/dev/null 2>&1 && echo 'gh present but the call failed' || echo 'gh not installed'))"
            rm -f "$OUT"
            {
              echo "cannot obtain ptdata $TAG. Tried, in order:"
              printf '%b\n' "$tried"
              echo
              echo "Fix whichever is easiest:"
              echo "  PTDATA_CHECKOUT=/path/to/ptdata   # a clone that has $TAG"
              echo "  ssh -T git@github.com             # confirm SSH access to GA-FDP"
              echo "  gh auth login                     # if you would rather use a token"
            } >&2
            exit 1
        fi
    fi
fi

# stat, not du: du reports allocated blocks and on this filesystem prints 512
# for a 1.8 MB file, which reads exactly like a truncated download.
echo "wrote $OUT ($(stat -c%s "$OUT") bytes)"

# Fail here rather than three minutes into a container build: a truncated or
# HTML-error-page "tarball" is a perfectly valid file and only announces
# itself when tar runs.
tar -tzf "$OUT" >/dev/null
echo "verified: readable gzip tar"
