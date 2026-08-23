# A Pelican-derived origin image, published to GHCR

Replacing a build-on-the-deploy-host pipeline with a pull. The origin runs an
image GA publishes, containing the plugins already compiled against the exact
Pelican base they load into.

Related: `docs/deployment-notes.md`, GA-FDP/xrdoss-mdsplus#5 (the point
endpoint), `d3d-origin-admin`.

## Problem

Deploying a plugin to the origin currently means building it there. From
`d3d-origin-admin/bin/build.sh`:

1. `git clone` xrdoss-mdsplus over SSH — GitHub credentials on the deploy host
2. `pixi run build` — the relay compiled with the **conda** toolchain
3. `scripts/fetch-ptdata-src.sh` — a second clone, of the **internal** ptdata repo
4. `podman build` for the mdsip sandbox image, pulling MDSplus RPMs over the network
5. `deliver-image.sh` — `podman save` / `load` the sandbox image
6. `install-relay.sh` — copy `.so` files into the origin's bind mount, restart

Six steps, three toolchains, two GitHub clones, and a version pinned in five
places at once (`site.env`'s `XRDOSS_REF`, `ARG PTDATA_VERSION` in two
Containerfiles, the fetch script's default, and the conda pins).

**Step 2 is not merely awkward; it is already wrong.** A conda-built plugin
requires `GLIBCXX_3.4.31`, and the AlmaLinux 9 origin image provides
`3.4.29`. The relay alone survives that because it is C++14 and needs only
`3.4.21`; the point endpoint links libptd3d, which is C++20, and crosses the
line. Measured:

| built by | max GLIBCXX required | loads on the origin? |
|---|---|---|
| conda toolchain | `3.4.31` | **no** |
| Pelican origin image's own gcc | `3.4.29` | yes |

So the deploy path in production today cannot build the point endpoint at all.
It would produce a plugin that fails to load, and the first symptom would be an
origin that does not start.

## Decision: publish a derived origin image

CI builds `FROM` the Pelican origin base, compiles both plugins in-image, and
publishes the result. The origin pulls it and runs it in place of the stock
image.

```
ghcr.io/ga-fdp/fdp-origin:<pelican-version>-fdp<our-version>
  FROM hub.opensciencegrid.org/pelican_platform/origin@sha256:<digest>
  + /plugins/libXrdOssMdsplus-5.so     MDSplus static-linked
  + /plugins/libXrdHttpMdsip-5.so      relay + point endpoint, libptd3d static
```

and, from the same pipeline, `ghcr.io/ga-fdp/fdp-mdsip:<version>` for the
sandbox, which is a genuinely separate running service.

### Why derived rather than an artifact image

The alternative considered was shipping an image containing only the two `.so`
files, extracting them on the origin, and leaving the stock Pelican image in
place. It looks cheaper — 3 MB per deploy against ~900 MB, and no
responsibility for a base image.

It is not actually cheaper, because **the coupling exists either way**. The
plugins are compiled against the base image's ABI and currently sit at
`GLIBCXX_3.4.29` with *no headroom*: the exact ceiling AlmaLinux 9 provides.
Any Pelican base change that moves libstdc++ decides whether they load. An
artifact image does not remove that revalidation, it only makes it invisible
until the origin fails to start.

A derived image states the truth — these plugins are valid against one base —
and makes CI prove it at build time rather than production discover it at
restart.

The cost accepted with open eyes: GA now owns rebasing on Pelican releases,
and a Pelican security update does not reach the origin until we publish.

## What is in the image, and what is not

**In:** the two plugins, and nothing else. The base is otherwise untouched.

**Out: all configuration.** Hostnames, archive paths, `authpath` values, the
mdsip host and port are site-specific; baking them in would make the image
unshareable and force a rebuild for a config change. The `http.exthandler`
line stays a bind-mounted fragment exactly as it is today, and
`d3d-origin-admin` keeps generating it from `site.env`.

**Pin the base by digest, not by tag.** With `:latest`, the same image tag
builds differently on different days and "which Pelican is in production"
becomes unanswerable. The tag carries the readable version for humans; the
digest makes the build reproducible. Rebasing is then an explicit,
reviewable change to one line.

## Registry and access

**GHCR, private packages under the GA-FDP organisation**, with a
`read:packages` token on the origin.

Verified rather than assumed — from `d3d-origin.gat.com` itself:

```
https://github.com                              HTTP 200
https://ghcr.io/v2/                             HTTP 401   (correct anonymous answer)
https://pkg-containers.githubusercontent.com    HTTP 400   (blob storage)
https://objects.githubusercontent.com           HTTP 404
anonymous token: obtained
PULL OK (2.12.3) -- manifest and blobs both transferred
```

Two things worth recording about that check. The origin **pulls directly**;
no hop through `d3d-dtn1` is needed, so `deliver-image.sh` can go away rather
than being re-pointed. And the third line is the one that matters: `ghcr.io`
serves manifests and redirects, while layers come from
`pkg-containers.githubusercontent.com`. A firewall rule allowing only
`github.com` yields a pull that authenticates, starts, and stalls on the first
blob — so that host must be tested explicitly, and is.

A `read:packages` token is strictly narrower than what the pipeline needs
today, which is SSH clone access to two repositories including an internal
one. This satisfies the standing constraint that the mdsip service account not
be given GitHub access, rather than working around it.

Size is not a constraint: GHCR limits individual layers (~10 GB), not images,
and GA-FDP is on an Enterprise plan, where private package storage and transfer
draw on a large allowance with billable overage rather than a hard cap. Worth
confirming against current billing, since those figures move.

## What CI must prove before publishing

The assertions currently living in `Containerfile.build` become release gates:

- no `NEEDED` entry for `ptd3d`, `curl`, `fdpio`, `XrdCl`, or MDSplus — the
  plugins are self-contained and cannot reach the network through libptd3d,
  which is built `PTDATA_WITH_FDPIO=OFF PTDATA_WITH_HTTP=OFF`
- `XrdHttpGetExtHandler` is defined — a plugin that loads and exports nothing
  fails exactly like a data problem
- the maximum required `GLIBCXX` is provided by the image's own
  `libstdc++.so.6` — the check that makes the whole approach worth having
- the ptdata source is the release the Containerfile claims, via the
  `export-subst` tag embedded in `python/ptdata/_version.py`

And one addition now that everything is containerised: **start the built image
and run the client contract suite against it**
(`tests/integration/test_point_endpoint.sh`). That is the same set of
assertions GA-FDP/ptdata makes against its stub, so a published image is one
that has actually served a point record correctly.

## Deploying

Before: clone, build, save, load, copy, restart.

After:

```bash
podman login ghcr.io            # once, with a read:packages token
podman pull ghcr.io/ga-fdp/fdp-origin:7.23.3-fdp2.3.0
# update the image ref in the origin's systemd unit / quadlet
systemctl restart pelican-origin
```

Rollback is the previous tag, still in the registry — an image reference
change rather than rebuilding an old commit on a host whose podman may or may
not be healthy that morning.

`d3d-origin-admin` loses `build.sh` and `deliver-image.sh` entirely.
`install-relay.sh` becomes "set the tag, pull, restart". The deploy host needs
no pixi, no compiler, no ptdata source, no MDSplus RPMs, and no GitHub clone
credentials.

## Risks

- **Base currency.** GA now stands between Pelican's releases and the origin.
  See Deferred.
- **Pelican may change how the origin image expects to be run** — an entrypoint
  or config-path change would surface as a derived image that no longer starts.
  Pinning by digest means this can only happen at a deliberate rebase, where
  the validation suite runs.
- **One more registry credential to rotate** on the origin.
- **omega06's podman is unreliable** — image stores there have twice lost layer
  contents overnight (see `deployment-notes.md`). This design removes that host
  from the deploy path, but anyone building locally still meets it.

## Deferred

**How a rebase is triggered is not designed here**, at the user's direction,
and it is the main cost of choosing a derived image: until something watches
for new Pelican releases, base updates reach the origin when a person notices.
That is a known shape in this ecosystem — workflows fire on commits and PRs, so
an upstream release changes nothing until someone looks. A scheduled job that
resolves the current base digest, rebuilds on a change, and runs the validation
suite would close it.

## Out of scope

- Migrating the mdsip sandbox's *runtime* arrangement; only where its image
  comes from changes.
- The origin's xrootd configuration, which stays external and site-specific.
- Anything about the point endpoint's own behaviour, which is #5.
