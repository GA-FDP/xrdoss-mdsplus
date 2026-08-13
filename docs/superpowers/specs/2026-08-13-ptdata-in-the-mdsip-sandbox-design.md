# PTData in the mdsip sandbox

**Status:** design, approved 2026-08-13. Not yet implemented.

## The problem

DIII-D trees contain nodes whose data is fetched by an embedded TDI call —
`PTDATA2(\ECPTNAM[F][i], $SHOT, 4)` and similar. The `ece` tree alone has 96 of
them. In the sandbox those nodes fail today, and they fail for two independent
reasons.

**The functions are missing.** The sandbox installs the upstream MDSplus RPM
(7.158-2), which ships 182 `.fun` files and none of DIII-D's. The site functions
— 119 of them, including `ptdata2.fun`, `pthead2*.fun` and `ptdata_library.fun`
— live in `DIII-D/css-d3d-mdsplus` under `tdi/`, which production's `MDS_PATH`
points at. Without them a node calling `PTDATA2` fails with
`%TDI-E-UNKNOWN_VAR`, the same failure the Containerfile already guards against
for `GetManyExecute.fun`.

**The data is unreachable.** `PTDATA2` reaches data through
`BUILD_CALL(0, PTDATA_LIBRARY(), "ptdata_", ...)` — an FFI call into a native
library named by the `PTDATA_LIBRARY` environment variable. The sandbox has no
such library, and by design no network: `--internal`, `--dns=none`, no outbound
TCP, all asserted by `tests/security/verify_sandbox.py`.

## What makes this tractable

The origin container mounts `/mnt/beegfs/data` at `/fdp-d3d`, and
`/fdp-d3d/archives/` contains `mdsplus`, `ptdata`, `index` and `incoming`. The
ptdata shotfiles and their indexes are therefore on the **origin host's local
filesystem**, beside the tree archive the sandbox already bind-mounts read-only.

`ptdata` builds its shared library with an option:

```cmake
option(PTDATA_WITH_FDPIO "Enable libfdpio backend" ON)
```

With it OFF the library builds "without remote file support" and links only
`${CMAKE_DL_LIBS}`. `io/local_io_provider.cpp` is compiled unconditionally;
`io/fdpio_io_provider.cpp` only when the option is ON. So a ptdata built for the
sandbox is *physically incapable* of remote I/O — the no-network property holds
by construction, not only by network policy.

## Decisions

| Question | Decision | Why |
|---|---|---|
| Shot coverage | Archived shots only | Keeps the sandbox network-free; recent-shot access would mean reopening the exfiltration channel the sandbox exists to close |
| TDI function source | `DIII-D/css-d3d-mdsplus`, pinned ref | It is what production's `MDS_PATH` uses; the fork's `tdi/d3d` is a mirror that has already drifted once |
| Function scope | All of `tdi/`, not the ptdata subset | These functions call each other; a missing one fails as `%TDI-E-UNKNOWN_VAR` at query time |
| Shotfile resolution | Index only, no `SYS_D3` scan | A directory scan is too slow |
| Index path format | POSIX resolution trick (below) | Contained entirely in the sandbox; no cross-repo change on the path to deployment |
| ptdata floor | `>=2.0.15` | That release initialises `ier=0` in `ptfile()`, fixing a `PTSEARCH0` infinite loop in `SYS_D3`-less environments — and this deployment is `SYS_D3`-less |

## Design

### 1. TDI functions

Clone `DIII-D/css-d3d-mdsplus` at a pinned ref during the image build, copy
`tdi/` to `/usr/local/d3d/tdi`, and extend `MDS_PATH` with all six directories:

```
/usr/local/d3d/tdi
/usr/local/d3d/tdi/ptdata
/usr/local/d3d/tdi/ptdata2
/usr/local/d3d/tdi/ptdata_historic
/usr/local/d3d/tdi/global
/usr/local/d3d/tdi/nimrod
```

`MDS_PATH` is a flat list, not recursive. Naming only the parent resolves
`PTDATA2` while leaving `PTHEAD2` unresolved — a partial install that looks like
a data problem rather than a packaging one.

### 2. The ptdata library

A multi-stage build compiles `libptd3d.so` and `libjson_index_plugin.so` with
`-DPTDATA_WITH_FDPIO=OFF`, and copies **only** the shared objects into the
runtime image. No compiler, no `libfdpio2`, no XRootD reaches the sandbox.

```
PTDATA_LIBRARY   /usr/local/ptdata/lib/libptd3d.so
```

Verify at build time whether `libgfortran` is required at runtime; the ptdata
test targets link it explicitly.

### 3. Index-based resolution

```
PTDATA_PLUGIN_LIB          /usr/local/ptdata/lib/libjson_index_plugin.so
PTDATA_JSON_INDEX_DIR      /ptdata-index
PTDATA_JSON_INDEX_PATTERN  json_indexes_*
```

The plugin reads `<index_dir>/<shot/100>/<shot>.json` through an `IoProvider`;
with fdpio compiled out that is `LocalIoProvider`, plain POSIX I/O.

### 4. Mounts

Two additional read-only bind mounts, mirroring the existing tree mount:

| Host | Container | Contents |
|---|---|---|
| `/mnt/beegfs/data/archives/ptdata` | `/fdp-archives/archives/ptdata` | shotfiles (`ptdata1`, `ptdata2`, … `ptdatae`) |
| `/mnt/beegfs/data/archives/index/json` | `/ptdata-index` | `json_indexes_<timestamp>/` snapshots |

The shotfile mount is deliberately nested under `/fdp-archives/archives/ptdata`
rather than somewhere flat: §5's symlink points at `/fdp-archives`, and the path
below it has to match what the index records. Mounting only `.../ptdata` (rather
than its parent) keeps the rest of `/mnt/beegfs/data` out of the sandbox.

The index needs no such treatment. `PTDATA_JSON_INDEX_DIR` is a value we set
ourselves, so it is an ordinary local path; only the shotfile locations *inside*
the index entries carry Pelican URLs.

`site.env` gains the host paths and the environment block, in the same shape as
`ARCHIVE_ROOT` and `MDSIP_TREE_ENV`, so the deployment stays described in one
file.

### 5. The Pelican-path workaround — REVISIT THIS

Index entries record **absolute Pelican URLs**, not paths:

```json
".PCE": "pelican://osg-htc.org:443/fdp-d3d/archives/ptdata/ptdatac/19887x/198873.PCE"
```

`LocalIoProvider::resolve()` returns its argument unchanged, so that string
reaches `::open()` verbatim and fails.

The workaround exploits POSIX path resolution and needs no code anywhere. The
string does not begin with `/`, so it is a **relative** path, and POSIX collapses
consecutive slashes. With the working directory at `/`, it resolves as:

```
/pelican:/osg-htc.org:443/fdp-d3d/archives/ptdata/ptdatac/19887x/198873.PCE
```

So the image carries that directory chain, with the final component a symlink to
the root the index paths are relative to:

```
/pelican:/osg-htc.org:443/fdp-d3d  ->  /fdp-archives
/fdp-archives/archives/ptdata          ro mount (§4)
```

The index records `/fdp-d3d/archives/ptdata/...`, so everything below the
symlink must reproduce that layout — hence the nested mount point rather than a
flat one.

Verified working: opening the literal URL string succeeds through both `open(2)`
and Python's `open()` when the chain exists relative to the working directory.

Two requirements follow:

- **The working directory must be `/` explicitly.** socat's child inherits
  socat's cwd. `fdp-mdsip-connection` must `cd /` rather than rely on the image's
  default `WORKDIR`.
- **The Pelican hostname and federation prefix are encoded in a directory name.**
  If either changes, every ptdata lookup fails as "file not found" with nothing
  pointing at the cause.

> **Return to this.** The real fix is to make the indexer record paths relative
> to the archive root (`archives/ptdata/...`) and have each consumer prepend its
> own root — which is effectively what Pelican clients already do. That removes
> the trick, removes the hostname from the image, and makes the index portable
> between access methods. It was deferred because it requires regenerating the
> index and updating its consumers, which is not on the path to this deployment.

## Accepted consequences

**Coverage equals the index's coverage.** A shot whose shotfiles are on disk but
absent from the index is unavailable. The newest index snapshot observed is
`json_indexes_2026-06-23`, so the gap is real, not theoretical.

**A miss returns empty, not an error.** `ptdata2.fun` passes through only four
error codes:

```tdi
IF ( NE(_error,0) && NE(_error,2) && NE(_error,31) && NE(_error,33)) { return([0]); }
```

Anything else yields `[0]`. That function is site code we do not own, so a miss
reaches the client as an empty array rather than an exception. Considered and
accepted; a staleness signal was explicitly dropped from scope.

## Security

Posture is unchanged.

- The data added is already inside the boundary: any token holder may read
  everything under `/fdp-d3d/archives` read-only, so a client with code execution
  in the sandbox gains nothing it could not already fetch through the origin.
- Both new mounts are read-only.
- No network is added, and the ptdata library is built unable to open one.
- No new syscalls: this is file I/O the profile already permits. The seccomp
  capture should still be re-run — it is owed for the socat entrypoint anyway.

## Testing

- A fixture tree node with an embedded `PTDATA2()` call, so the existing
  sandboxed e2e covers the real path end to end.
- A check that fails loudly when `MDS_PATH` resolves `PTDATA2` but not its
  helpers — the partial-install case, which otherwise presents as missing data.
- A check that the Pelican-path chain resolves, and that it fails *loudly* if the
  working directory is not `/`. This is the fragile part of the design and
  deserves a test that names it. Note when writing it: the symlink is absolute
  (`-> /fdp-archives`), which only resolves inside the container where that path
  exists at the root. A test running on the host must either exercise it through
  the container or use a relative symlink; a host test with the absolute one
  fails with `ENOENT` for reasons unrelated to the design.
- The existing no-network assertions must still pass with ptdata present, and
  the built `libptd3d.so` must have no `libfdpio2` or `libXrd*` in its `NEEDED`
  entries — the same `readelf` check the packaging uses, for the same reason.

## Follow-ups

1. Make the indexer record archive-relative paths and retire §5's trick.
2. Re-run `tests/security/capture_syscalls.sh` against the socat entrypoint.
3. Consider whether `toksearch`'s `MdsSignal` should surface "empty result from a
   PTDATA2-backed node" distinctly, given the swallowed-error behaviour above.
