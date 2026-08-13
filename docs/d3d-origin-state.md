# d3d-origin: measured state, decisions, and what is left

**As of 2026-08-13.** A self-contained snapshot of the production origin as it
actually is, the deployment decisions taken and why, and the open items. Written
as the seed for a d3d-origin admin repo.

Everything under "measured" was observed on the host or reproduced locally
against the same image. Anything inferred is labelled as such.

---

## 1. The host

| | |
|---|---|
| Host | `d3d-origin.gat.com`, RHEL 9.6, kernel 5.14.0-570 |
| podman | 5.4.0, **netavark** 1.14.1 + aardvark-dns, crun, seccomp enabled |
| cgroups | **v2**, rootless, delegated controllers **`memory` and `pids` only — no `cpu`** |
| SELinux / AppArmor | **both disabled** |
| Rootless uid | 1122 (`sammuli`), subuid `362144:65536` |
| Hardware | 48 cpus, 200 GB RAM |

Consequences: the sandbox's `--memory` and `--pids-limit` **are** enforced here
(unlike the dev box), `--cpus` is **not** and is only warned about, and there is
no MAC layer beneath the container.

## 2. The origin container

Runs **rootful** (`sudo podman`), name `pelican-origin`:

```
/usr/bin/podman run --restart always -d -p 8000:8000 -p 8443:8443 \
  -v /var/pelican/config/scitoken:/var/run/stash-origin-auth/ \
  -v /data/pelican/logs:/logs \
  -v /var/pelican/jwks:/etc/pelican/jwks \
  -v /var/pelican/certs:/etc/pelican/cert-orig/ \
  -v /mnt/beegfs/data:/fdp-d3d/ \
  -v /var/pelican/config:/etc/pelican \
  -v /var/pelican/shoveler:/etc/pelican/shoveler \
  --name=pelican-origin --replace \
  hub.opensciencegrid.org/pelican_platform/origin:v7.23.3 \
  serve -f https://osdf-director.osg-htc.org
```

| Fact | Why it matters |
|---|---|
| Image `origin:v7.23.3`, **XRootD v5.9.1** | our plugin is built against **5.9.2**; verified it loads and serves anyway in this exact image |
| `-v /var/pelican/config:/etc/pelican` | **the whole config dir is a host bind mount**, so files can be added without touching the `podman run` |
| `-v /mnt/beegfs/data:/fdp-d3d/` **rw=true** | the origin has **write** access to the archive |
| `-p 8000:8000` | **port 8000 is taken**; the sandbox cannot use its default |
| `serve -f https://osdf-director.osg-htc.org` | federated with the **production** OSDF, not a test director |
| no `PELICAN_*` / `XROOTD_*` env | configuration is entirely file-based |
| `host.containers.internal` → **`10.88.0.1`** | bridge gateway, host-local — the address the relay must use |
| `localhost` inside the container | is the **container**, not the host (measured) |

## 3. Configuration files

All on the host under `/var/pelican/config/`, appearing as `/etc/pelican/` inside.

**`pelican.yaml`** (2318 B) sets `Xrootd.ConfigFile: /etc/pelican/xrootd.cfg`
— already claimed, so the relay line is *appended* to that file rather than
introducing a drop-in. There is **no `config.d/`**.

Its single export:

```yaml
Origin:
  StorageType: posix
  Port: "8443"
  EnableIssuer: true
  EnableOIDC: true
  Exports:
    - FederationPrefix: "/fdp-d3d"
      StoragePrefix: "/fdp-d3d"
      Capabilities: ["Reads", "Writes", "Listings"]      # note: NO PublicReads
      IssuerUrls:
        - https://fdp-d3d-origin.nationalresearchplatform.org:8000
        - https://t.nationalresearchplatform.org/fdp
        - https://osg-htc.org/osdf/fdp
```

**No `PublicReads` is load-bearing**: reads require a token, so `auth=xrootd`
has real policy to delegate to and the deny path is testable on the real origin.

`/fdp-d3d` already carries `Writes` — true today, independent of this work.

**`xrootd.cfg`** (40 B), the entire current contents:

```
xrootd.tls capable all
sec.protocol ztn
```

`ztn` is XRootD's token protocol, which is what `auth=xrootd` ultimately
delegates to. There is also a `xrootd.cfgOLD` (2241 B) — historical, unused.

## 4. The archive

`/mnt/beegfs/data/archives/mdsplus` on **BeeGFS**.

- **World-readable**, which is exactly what rootless podman's uid mapping needs
  — no ACL or group grant required.
- `setfacl` **fails** on BeeGFS (`Operation not supported`) unless ACLs are
  enabled server-side and remounted. Do not plan around ACLs here.
- Adding the service account to a group would **not** have worked either: the
  container reads as the mapped subuid, which is in no groups.

Tree search path, from `toksearch_d3d/toksearch_d3d/data/d3d.yaml`, with the
Pelican root swapped for the local one (`fdp` joins these with `;` —
`fdp/environment.py:148`):

```
/mnt/beegfs/data/archives/mdsplus/codes/~t/~j~i/~h~g/~f~e/~d~c
/mnt/beegfs/data/archives/mdsplus/usershots/~t
/mnt/beegfs/data/archives/mdsplus/models/~t
/mnt/beegfs/data/archives/mdsplus/shots/~t/~f~e/~d~c
```

## 5. Decisions taken, and why

**Deploy the relay only, first.** `libXrdHttpMdsip.so` links no MDSplus at all
(`libXrdUtils`, `libXrdHttpUtils`, libc) and was verified loading and serving a
full session in the **stock** `origin:v7.23.3`, which contains no MDSplus. The
Oss plugin is what forces a derived image, because `ConnectToMds` `dlopen`s
`libMdsIpTCP.so` by name — defer it.

**Address the origin directly; do not register a `/mdsip` namespace.** The
director only routes `PUT`, and only to namespaces with `Writes` — so a
federation route would mean a writable namespace in the *production* OSDF. Going
direct needs no namespace at all, because the ext handler claims the path before
XRootD consults any namespace. What is given up is director origin-selection,
which relay sessions cannot use anyway (they are sticky to one origin).

Clients therefore use:

```python
MDSplus.Connection('fdp://fdp-d3d-origin.nationalresearchplatform.org:8443/mdsip')
```

**`authpath=/fdp-d3d/archives/mdsplus`.** The tree root, so the question asked
is exactly "may you read the trees?". Also the more permissive sensible choice:
a token scoped narrowly to the archive satisfies it, while `/fdp-d3d` would
demand read on the namespace root and refuse that same token.

**Sandbox on `10.88.0.1:8100`.** Port 8000 is taken by the origin; `127.0.0.1`
is unreachable from the origin container; `0.0.0.0` would expose unauthenticated
code execution to the network. `10.88.0.1` is the bridge gateway — reachable by
the origin container and anything else on that bridge, and nothing beyond the
host. Rootless podman is permitted to bind it (verified).

**Mount the archive at its real path inside the sandbox**, so the tree path
needs no rewrite. The mount path is *not* a security control:
`/proc/self/mountinfo` leaks the bind source wherever it is mounted, and a
client with code execution can read it (measured).

## 6. The change set (not yet applied)

Nothing below has been done on d3d-origin yet.

1. **Service account** (root, once):
   ```bash
   useradd --system --create-home --home-dir /var/lib/fdp-mdsip --shell /bin/bash fdp-mdsip
   usermod --add-subuids 755360-820895 --add-subgids 755360-820895 fdp-mdsip
   loginctl enable-linger fdp-mdsip
   ```
   `755360` is the next free block; existing entries run 100000–755359.

2. **Image** into that account's storage — rootless podman keeps images per
   *user* and per *host*. Either build it there, or `podman save` / `scp` /
   `podman load` (the tar is 0644 and round-trips; verified).

3. **Start the sandbox** as `fdp-mdsip`, with `MDSIP_PUBLISH=10.88.0.1:8100`,
   `MDSIP_TREE_MOUNT` and `MDSIP_TREE_ENV` per §4, and `MDSIP_ORIGIN_UID=1122`.

4. **Plugin + config** (root): copy `libXrdHttpMdsip-5.so` into
   `/var/pelican/config/`, and append to `/var/pelican/config/xrootd.cfg`:
   ```
   http.exthandler mdsip /etc/pelican/libXrdHttpMdsip.so prefix=/mdsip,host=10.88.0.1,port=8100,auth=xrootd,authpath=/fdp-d3d/archives/mdsplus,idle=300,timeout=60
   ```
   Library name **unsuffixed** — XRootD appends the `-5` itself.

5. **Restart** the origin.

### Verification, in order

```bash
MDSIP_ORIGIN_UID=1122 pixi run sandbox-verify      # containment; expects 0 waived here
MDSplus.Connection('10.88.0.1:8100')               # real trees, before touching the origin

# after the origin restart -- BOTH directions matter
curl -k -X PUT --data-binary '' https://localhost:8443/mdsip/connect                    # expect 401
curl -k -X PUT --data-binary '' -H "Authorization: Bearer $TOK" https://localhost:8443/mdsip/connect  # expect a 32-char token
```

**If the first curl returns 200, stop.** That means authorization is not being
delegated and the relay is open to anyone who can reach port 8443.

## 7. Open items

| Item | State |
|---|---|
| Admin repo + script for the above | **next task**; must run as `sammuli`, and must not assume `fdp-mdsip` has GitHub access |
| Everything in §6 | not applied |
| Oss plugin (virtual files) | deferred; needs an image carrying the MDSplus runtime |
| Client transport packaging | `libMdsIpFDP.so` built and tested, not packaged for `ga-fdp` |
| Ext handler slots | Pelican already loads 3 of XRootD's 4; ours is the last |
| microVM isolation | considered, weaker case since the service-account split; `/dev/kvm` present |

## 8. Traps worth not rediscovering

**omega06 (the dev box), not the origin:** `/local-scratch` and `/tmp` are
purged periodically. That breaks podman two ways — container storage loses its
layers (every image unrunnable, `/bin/sh: no such file or directory`; fix is
`podman system reset` and rebuild), and `$XDG_RUNTIME_DIR/libpod/tmp` disappears
(fix is to recreate it; podman does **not** create it under a runtime dir handed
to it, and `podman system migrate` does not help). Leaving `XDG_RUNTIME_DIR`
unset also works there.

**`podman logs` on a container running for days can take minutes** and reads as
a hang. Use `--tail` and a `timeout`.

**Rootless podman keeps images per user *and* per host.** An image built on a
workstation does not exist on the origin: `Error: ... image not known`.

**A pipeline's exit status is the last command's.** `cmd | grep -q …` under
`pipefail` reports a false negative when grep closes the pipe early, and
`cmd | … | head || echo fallback` never fires the fallback. Both bit real
scripts here.
