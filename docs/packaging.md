# Packaging

Two conda packages on the **`ga-fdp`** channel, from this one source tree.

| Package | Ships | For | Depends on |
|---|---|---|---|
| `mdsip-fdp` | `lib/libMdsIpFDP.so` | anyone with an MDSplus thin client | MDSplus, libcurl |
| `xrdhttp-mdsip` | `lib/libXrdHttpMdsip-5.so` | origin administrators | `xrootd <6` |

The virtual-file plugin (`libXrdOssMdsplus-5.so`) is not packaged: it has no
deployment yet, and packaging something with no consumer just creates a version
to keep current.

```bash
pixi run package               # -> ~/outdir
./build_package.sh /some/dir
```

## Installing the client

```bash
conda install -c ga-fdp -c conda-forge mdsip-fdp
```

Then, with no environment variables at all:

```python
from MDSplus import Connection
conn = Connection('fdp://fdp-d3d-origin.nationalresearchplatform.org:8443/mdsip')
conn.openTree('efit01', 198873)
conn.get(r'\ipmhd')
```

A bearer token is read from `$BEARER_TOKEN` or `~/.fdp/token`.

**No `LD_LIBRARY_PATH`, and no `MDSPLUS_LIBRARY_PATH`.** MDSplus resolves a
transport by `dlopen`-ing `"lib" + "MdsIp" + SCHEME + ".so"`, and the `dlopen`
happens inside `libMdsShr.so`, whose `RPATH` is `$ORIGIN/.` — the directory the
package installs into. So the library is found because it is a neighbour of the
MDSplus libraries, which is the entire install requirement.

This is also why the file must be named **`libMdsIpFDP.so` exactly**. The XRootD
plugins take the opposite rule: `XrdOucPinLoader` appends the plugin version, so
`libXrdHttpMdsip-5.so` on disk is referenced *unsuffixed* in `xrootd.cfg`. Both
names are checked by the package tests, in both directions.

## The relay package

`xrdhttp-mdsip` exists so the origin's plugin can come from a versioned,
CI-built artifact rather than from whatever a build host resolves. It is built
against **XRootD 5.9.2**, the version in the Pelican origin image.

The origin does not run a conda environment, so consuming it means extracting
the file:

```bash
conda create -p /tmp/relay -c ga-fdp -c conda-forge xrdhttp-mdsip
cp /tmp/relay/lib/libXrdHttpMdsip-5.so /var/pelican/config/
```

**It does not replace `d3d-origin-admin/bin/build.sh` as things stand.** That
script produces three artifacts from one source checkout — the relay, the mdsip
sandbox image, and the seccomp profile — and only the first is packaged here.
Switching just that one over would leave the clone in place for the other two
and split the provenance across two mechanisms, so the source build remains the
supported path until the image and profile are packaged too.

## Why two MDSplus variants

`mdsip-fdp` is built twice, once against conda-forge's `mdsplus` and once
against ga-fdp's `mdsplus-xrdcl`, selected by `mdsplus_impl` in
`recipe/conda_build_config.yaml`.

The two MDSplus packages install the same files, so they cannot coexist — and
both are genuinely in use. `fdp-core` blesses `mdsplus-xrdcl`, because tree
files are read over Pelican there; a standalone client that only wants the
`fdp://` transport has no reason to want the fork, and pulling it in would drag
`libfdpio2` → `xrootd` → `xrdcl-pelican-fdp` behind it. A variant per provider
lets the solver pick rather than forcing one on everyone.

The two builds produce the same binary; only the metadata differs. The `-5`
XRootD plugin does not reference the variable, so it builds once — three build
configurations, not four.

## What the tests check

The package tests cover what *packaging* can break, which is not what the unit
tests cover.

**Resolution, with a positive control** (`recipe/test_transport.py`). If MDSplus
cannot find a transport it does not report an error — `LoadIo` silently returns
the ssh-tunnel routines instead. So "the connection failed" cannot distinguish a
missing plugin from a missing network, and the check has to ask `LoadIo`
directly:

```
LoadIo(unknown) -> 0x…820   (the fallback)
LoadIo(tcp)     -> 0x…020   positive control: MDSplus can load anything at all
LoadIo(fdp)     -> 0x…020   must differ from both
```

Without the `tcp` control, a `LoadIo` that returned `NULL` for everything would
pass. The test re-execs itself with `LD_LIBRARY_PATH` and
`MDSPLUS_LIBRARY_PATH` removed before any of this, because glibc reads those
once at process start and clearing them in-process would not affect a later
`dlopen` — the claim is that none is needed, so none may be inherited.

**A live fetch**, skipped (loudly) without `BEARER_TOKEN`, since fork PRs cannot
see repo secrets.

**Dependency shape**, from `NEEDED` entries — the client must not link XRootD,
and the relay must not link MDSplus, those being the two claims that make the
split worth making. Read from `readelf`, not `ldd`: `ldd` prints resolved paths
containing the package name, and `grep -i mds` against those matches
`xrdhttp-mdsip`, failing a perfectly clean binary. It did exactly that on the
first build.

## Versions

`build_package.sh` derives the version from `git describe`, versioneer-style, so
a package always names the commit it came from:

| Situation | Version |
|---|---|
| on a `release-*` tag | `0.1.0` |
| 3 commits past one | `0.1.0+3.g1a2b3c4` |
| uncommitted changes | `0.1.0+3.g1a2b3c4.dirty` |
| no tags yet | `0.0.0+g1a2b3c4` |

`.dirty` is deliberate: a package built from a modified tree is not the tag it
claims, and the version string is a better place to learn that than the
behaviour.

Releases are cut by tagging `release-X.Y.Z` — CI builds and uploads to `ga-fdp`
only for those tags. Note that the build string carries the variant hash, so it
must not be overridden in the recipe; doing so would give both `mdsplus_impl`
variants the same filename.

## Independent builds

Each package builds only its own plugin (`-DBUILD_CLIENT`, `-DBUILD_RELAY`,
`-DBUILD_OSS`), and CMake only probes for the dependencies the selected targets
need. This is what makes the split real rather than cosmetic: the relay's build
environment contains no MDSplus, so the claim that it links none is enforced by
the build, not just asserted afterwards.

`BUILD_TESTS=OFF` in both, so the unit tests never run during a package build —
CI runs `pixi run test` as a separate job for that.
