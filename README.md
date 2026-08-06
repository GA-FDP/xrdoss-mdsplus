# xrdoss-mdsplus

An out-of-tree XRootD `XrdOss` plugin that serves MDSplus TDI evaluation
results as virtual files, plus the out-of-process evaluator it talks to.

Loaded into a **stock** Pelican origin — no fork — by pointing
`Xrootd.ConfigFile` at a fragment containing:

```
ofs.osslib ++ /path/to/libXrdOssMdsplus.so prefix=/tdi socket=/run/fdp/evald.sock
```

Note the name in the config has **no `-5` suffix** even though the file on disk
is `libXrdOssMdsplus-5.so`. XRootD appends the plugin version itself; referencing
the suffixed name makes it search for `-5-5.so` and only succeed via a fallback.

## Layout

| Path | What |
|---|---|
| `src/` | The plugin. Logic lives in small units (`Base64Url`, `TdiPath`, `EvalClient`, `ResultCache`); `OssMdsplus.cc` is thin wiring. |
| `evaluator/` | Python daemon that evaluates TDI and returns serialized MDSplus descriptors over a unix socket. Keeps MDSplus out of the XRootD process. |
| `tests/` | doctest unit tests, pytest for the evaluator |
| `tests/fed/` | Local Pelican federation in podman — see `tests/fed/FINDINGS.md` |
| `tests/integration/` | Standalone XRootD end-to-end |

## Building

```bash
pixi run build     # -> build/libXrdOssMdsplus-5.so
pixi run test      # doctest suites via ctest
pixi run pytest    # evaluator tests
```

XRootD is pinned `<6` to match the deployed v5 plugin chain. The Pelican origin
image ships XRootD v5.9.2 and the pixi environment resolves to the same version,
so the plugin is built against the ABI it is loaded into.

## Testing against a real federation

```bash
bash tests/fed/fedbox.sh start                    # plain federation
bash tests/fed/fedbox.sh start /tmp/extra.cfg     # with an osslib fragment
bash tests/fed/fedbox.sh start /tmp/extra.cfg /path/to/plugin.so
bash tests/fed/fedbox.sh stop
```

## Design

`repos/docs/superpowers/specs/2026-08-06-mdsplus-virtual-file-service-design.md`
and the implementation plan alongside it in `plans/`.
