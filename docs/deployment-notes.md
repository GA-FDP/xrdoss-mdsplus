# Deployment and environment notes

Things that cost time to discover and would cost it again.

## The origin image needs the MDSplus runtime

`ConnectToMds` calls `LoadIo("tcp")`, which **`dlopen`s `libMdsIpTCP.so` by
name** (`mdstcpip/mdsipshr/LoadIo.c:54-62`). No amount of static linking removes
that: a library discovered by filename cannot be linked in. When it is missing,
`LoadIo` silently falls back to `tunnel_routines` and the only symptom is
`ConnectToMds(...) failed`.

`Containerfile.runtime` builds a stock Pelican origin plus `mdsplus-kernel_bin`,
which ships the transport. MDSplus installs to `/usr/local/mdsplus/lib64`, which
is not on the default loader path, so the image also drops an
`/etc/ld.so.conf.d/mdsplus.conf` and runs `ldconfig`.

## mdsip run unprivileged needs `* | SELF`

`MAP_TO_LOCAL` makes mdsip attempt a setuid to the **client's** username. When
the client is XRootD inside the Pelican container that username is `xrootd`,
which does not exist on the host, and the connection is refused with **no
server-side log at all**.

`* | SELF` tells mdsip not to switch user (`CheckClient.c:76`). A local test
where the client happens to run as the same user as the server passes with
`MAP_TO_LOCAL` and hides this completely.

## `MDS_PATH` must include `tdi/remote`

`GetManyExecute` is a TDI function (`tdi/remote/GetManyExecute.fun`), not a
builtin. Without that path every request fails with `%TDI-E-UNKNOWN_VAR`, which
reads like a client bug and is not one.

## podman when `/run/user/$UID` is gone

On a host without a live systemd user session — after the login that created
`/run/user/$UID` ends, and `/run/user` is root-owned so it cannot be recreated —
podman fails with:

```
Error: creating events dirs: mkdir /run/user/1122: permission denied
```

`XDG_RUNTIME_DIR` alone does **not** fix it; several paths are derived
independently. What works is a `~/.config/containers/containers.conf`:

```toml
[engine]
events_logger = "file"
events_logfile_path = "/tmp/containers-user-<uid>/tmp/events.log"
tmp_dir  = "/tmp/containers-user-<uid>/tmp"
static_dir = "/local-scratch/<user>/containers/storage/libpod"
volume_path = "/local-scratch/<user>/containers/storage/volumes"
```

plus `XDG_RUNTIME_DIR` pointed at a writable 0700 directory for the rootless
pause process. `tests/fed/fedbox.sh`, `scripts/mdsip-sandbox.sh` and
`tests/security/verify_sandbox.sh` set the latter automatically when
`/run/user/$UID` is absent.

**Create `$XDG_RUNTIME_DIR/libpod/tmp` as well.** podman creates that itself
under a runtime directory it set up, but *not* under one handed to it, and the
failure names neither the variable nor the directory:

```
error creating temporary file: No such file or directory
ERRO invalid internal status, try resetting the pause process with
     "podman system migrate": setting up the process:
     open /tmp/xdg-1122/libpod/tmp/pause.pid: no such file or directory
```

`podman system migrate`, which the message recommends, does **not** fix it —
`mkdir -p "$XDG_RUNTIME_DIR/libpod/tmp"` does. Counter-intuitively, leaving
`XDG_RUNTIME_DIR` *unset* also works once the `containers.conf` above is in
place, because podman then picks and creates its own runtime directory. Setting
it to a hand-made path is what introduces the requirement.

Both this and the `containers.conf` paths live under `/tmp`, which is cleaned
periodically on omega06 — so these directories vanish and the symptom returns
without anything having changed. The same cleaning removes the container
storage under `/local-scratch`, which shows up separately as every image being
unrunnable (`/bin/sh: no such file or directory`) and needs
`podman system reset` plus a rebuild.

**Do not also override the OCI runtime.** Setting `runtime = "runc"` with an
`[engine.runtimes]` table produced `default OCI runtime "runc" not found:
invalid argument`, which masks the real errors and looks like a different
problem entirely. The default runtime is correct; only the paths need changing.

The durable fix is `loginctl enable-linger <user>`, which needs root.
