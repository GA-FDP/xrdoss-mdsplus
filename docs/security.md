# Security: what this service actually exposes

**Date:** 2026-08-10 · **Status:** sandbox built and verified; not yet deployed

Both plugins in this repo end at the same place: a client-supplied TDI
expression evaluated by an MDSplus server. This document records what that
means, measured rather than assumed, and what the sandbox does about it.

## The finding: TDI evaluation is arbitrary native code execution

Not "could be under some circumstances" — is, today, through documented
features, with no exploit involved. Measured against a stock mdsip by
[`tests/security/probe_capability.py`](../tests/security/probe_capability.py):

| What a client sent | What happened |
|---|---|
| `spawn("id")` | ran a shell command |
| `MdsShr->system("touch /tmp/ffi-proof")` | ran a shell command — **and the file appeared** |
| `MdsShr->getuid()` | returned the server's uid |
| `MdsShr->fork()` | returned a new pid |
| `spawn("touch $efit01_path/probe")` | wrote into the tree directory |
| `spawn("getent hosts example.com")` | resolved a public name |
| `spawn("</dev/tcp/1.1.1.1/53")` | opened an outbound connection |

Two of those deserve emphasis.

**`image->routine()` is a generic FFI, and it reaches libc.** `MdsShr->system(...)`
works because `dlsym` on a library handle searches that library's dependency
chain, and MDSplus links libc. So the FFI is not limited to MDSplus's own
symbols — it is a call gate to any function in any linked library.

**Therefore removing `spawn()` would accomplish nothing.** That was worth
checking before designing anything, because "just disable the dangerous
builtin" is the intuitive first move and it is a dead end. There is no subset of
TDI that is both useful and safe: evaluating expressions is the product.

This is not a defect report against MDSplus. TDI is a scientific scripting
language and an FFI is a reasonable thing for one to have. It simply means the
security boundary cannot be inside the mdsip process. **The container is the
boundary.**

## Threat model

**Attacker:** any holder of a valid FDP token. Per the operating assumption for
this phase, that is anyone who can read `/fdp-d3d/archives` — every token holder,
read-only. There is no privileged subset to protect data *from*.

**Capability:** arbitrary native code execution as the mdsip user, inside
whatever mdsip runs in. Assume it, do not try to prevent it.

**What is actually at risk**, in descending order:

1. **The origin's issuer keys.** A stolen `issuer.jwk` mints arbitrary
   federation tokens — total compromise of the federation, not just this
   service. This is the crown jewel and everything else is a rounding error
   next to it.
2. **The internal network.** The origin sits somewhere reachable. Code
   execution with egress is a pivot inward and an exfiltration channel outward.
3. **Tree integrity.** Read-only is the stated policy for archive data. A
   server that can write trees makes that policy a preference.
4. **Availability.** `fork()` works, so a fork bomb is two characters of TDI.
5. **The host.** Container escape via a kernel bug.

**Explicitly not in scope:** confidentiality *between* token holders. Everyone
may read everything, so the sandbox does not need per-user isolation.

That last point is what makes this tractable, and it is worth being explicit
about because it is where the JupyterHub analogy breaks down. JupyterHub needs a
container *per user* because users have different data rights and must not read
each other's notebooks. We need exactly **one** boundary — between "any client"
and "everything that is not archive data" — so a single shared mdsip container
is sufficient. If per-user data rights are ever introduced, that conclusion
changes and the design has to change with it.

## The sandbox

[`Containerfile.mdsip`](../Containerfile.mdsip) builds an image containing an
MDSplus server and nothing else;
[`scripts/mdsip-sandbox.sh`](../scripts/mdsip-sandbox.sh) supplies the isolation.
**The image without the flags buys nothing** — the controls are all in how it is
run.

| Control | Flag | Stops |
|---|---|---|
| Separate container from the origin | — | reaching issuer keys, tokens, Pelican config |
| No route off the host | `podman network create --internal` | pivot and exfiltration |
| No resolver at all | `--dns=none` | DNS as an exfiltration channel, whatever the backend does |
| Read-only root | `--read-only` | persistence, replacing the server binary |
| Read-only trees | `-v $TREES:/trees:ro` | tampering with archive data |
| Writable scratch that cannot execute | `--tmpfs /tmp:noexec,nosuid,nodev` | staging a binary |
| No capabilities | `--cap-drop=ALL` | most escape paths |
| No privilege regain | `--security-opt=no-new-privileges` | setuid escalation |
| Not uid 0 | `USER mdsip` (5000) | shortening the path out of the namespace |
| Dedicated service account | run rootless podman as `fdp-mdsip`, not the origin's user | container-root mapping to the owner of `issuer.jwk` |
| Bounded forks | `--pids-limit 256` | fork bombs |
| Bounded memory and CPU | `--memory 2g --cpus 2` | resource exhaustion |
| Process per connection | `mdsip -m` | one client's crash or hang staying its own |

Rootless podman adds a user namespace on top, so uid 0 *inside* the container is
an unprivileged uid outside it.

### Three things the verification found that reading documentation would not

**podman mounts the host's credentials into the container by default.** On
RHEL-family hosts `/run/secrets` arrives populated with the host's subscription
entitlement certificates and `rhsm` configuration — nobody configured this, and
a client with code execution can read them. The sandbox masks the path with an
empty tmpfs. Worth remembering as a general point: the container starts with
more than the Containerfile puts in it.

**`--pids-limit`, `--memory` and `--cpus` are silently ignored on cgroups v1
rootless.** podman accepts the flags, prints one line among its startup noise —
`Resource limits are not supported and ignored on cgroups V1 rootless systems` —
and runs the container with no limits at all. Since `MdsShr->fork()` works, that
is the fork-bomb control quietly absent.

The general rule, which the origin host demonstrates: **a limit is enforced only
if its controller is delegated**, and podman accepts the flag either way. On
cgroups v2 rootless, systemd commonly delegates `memory` and `pids` but not
`cpu` — the origin's controllers are exactly `[memory pids]` — so `--cpus` is
accepted and ignored there too. `mdsip-sandbox.sh` therefore asks which
controllers exist, passes only the flags they support, **refuses to start** if
`memory` or `pids` is missing unless `MDSIP_ALLOW_NO_LIMITS=1` accepts that
deliberately, and warns about `cpu`. The verifier reports absent limits as
WAIVED rather than passing over them.

**`mdsplus-kernel_bin` ships no TDI function library.** No `tdi/` directory at
all, so `GetManyExecute.fun` is absent and every batch request fails with
`%TDI-E-UNKNOWN_VAR` — from a server that starts and accepts connections
normally. The noarch `mdsplus-kernel` package carries it; the image installs
both, and the build asserts the file exists rather than discovering it in
production.

## Verifying it

A sandbox nobody attacks is indistinguishable from no sandbox at all, so the
controls are asserted by attacking them through ordinary TDI — the same channel
a real client has:

```bash
podman build -f Containerfile.mdsip -t fdp-mdsip .
pixi run sandbox-verify
```

[`verify_sandbox.py`](../tests/security/verify_sandbox.py) first confirms the
client *does* have code execution — if that check ever fails, the probe broke,
not MDSplus — and then asserts each containment property. It deliberately tests
the running server rather than podman's view of its own configuration, which
would only prove the flags were typed correctly. On this host, as of
2026-08-10:

```
17 checks passed, 2 waived
```

with the two waivers being the resource limits the host cannot enforce.

### Where that result was measured, and how the origin differs

It was obtained on **podman 4.9.4 / CNI / cgroups v1 / rootless**. The origin
(`d3d-origin.gat.com`) is **podman 5.4.0 / netavark 1.14.1 / crun / cgroups v2 /
rootless, RHEL 9.6**. Podman 5.0 removed CNI entirely, so the network results in
particular were measured against a backend the origin does not have. The
verifier prints the stack it ran on for exactly this reason: a run whose output
does not say where it ran is not evidence.

| Control | Transfers? |
|---|---|
| separate container, read-only root, `:ro` trees, noexec tmpfs, `/run/secrets` masked, `--cap-drop=ALL`, no-new-privileges, non-root uid, own pid namespace | **yes** — none depend on the network backend or cgroup version |
| no outbound TCP, no default route | needs re-running, but `--internal` means the same thing to both backends |
| no DNS | **now closed by construction** — see below |
| memory and pids limits | **better there**: cgroups v2 with `memory` and `pids` delegated, so the two waivers here become real limits |
| cpu limit | **not enforced** on the origin: its delegated controllers are `memory` and `pids` only |

**DNS turned out to need fixing, not checking.** The container is handed the
host's nameserver in `/etc/resolv.conf` *even on an `--internal` CNI network* —
measured: `nameserver 10.1.1.254`. The "no DNS resolution" check was passing
only because nothing could route to it, not because DNS was off. Netavark's
aardvark-dns sits on the bridge, which an internal network **can** reach, so the
same configuration would likely resolve names on the origin. DNS alone is an
exfiltration channel.

Rather than rely on backend behaviour, the sandbox now passes **`--dns=none`**
unconditionally: an empty `resolv.conf`, lookups failing immediately, and no
loss of function because mdsip connects to nothing. Verified not to break the
service end to end.

**The `cpu` controller is not delegated on the origin** (`cgroupControllers:
[memory, pids]`), so `--cpus` would be accepted and ignored. The script now
checks which controllers actually exist and passes only the flags those
support, warning about `cpu` rather than refusing — memory and pids bound the
catastrophic cases, while a missing cpu limit means a client can burn cores.
Fix with `systemctl edit user@$(id -u).service` and `Delegate=memory pids cpu`.

Two things still to confirm there, both cheap and both answered by running the
verifier:

- **Host reachability.** CNI's internal bridge gets no host-side IP at all
  (measured: a host listener on `0.0.0.0` was unreachable from the sandbox).
  Netavark assigns a gateway address, so that margin should not be assumed.
- **Rootless networking.** The origin's default rootless mode is pasta, not
  slirp4netns. Named bridge networks should be unaffected, but confirm port
  publishing works at all.

## Run it as a dedicated service account

**Rootless podman maps container uid 0 to the invoking user.** Measured on a
running sandbox:

```
$ podman exec fdp-mdsip cat /proc/self/uid_map
         0       1122          1          <- container-root IS the invoking user
         1     886432      65536
```

The server itself is already well separated: it runs as container uid 5000,
which maps high into the subuid range (host 891431 above), and an escape from
*that* cannot read uid 1122's files. The exposure is narrower and worse: if a
client ever becomes **container-root**, it becomes the account that owns the
origin's `issuer.jwk`. Today that is blocked by `--cap-drop=ALL`,
no-new-privileges, a non-root uid and no setuid binaries — four controls
holding one door shut.

Running the sandbox as a **separate service account** removes the door. Then
container-root maps to an account that owns nothing, and the crown jewel is
protected by ordinary file permissions rather than by every one of those
controls continuing to hold.

```bash
# as root, once
useradd --system --create-home --home-dir /var/lib/fdp-mdsip \
        --shell /sbin/nologin fdp-mdsip
# system accounts get no subuid range automatically; pick one that does not
# overlap an existing entry in /etc/subuid
usermod --add-subuids 900000-965535 --add-subgids 900000-965535 fdp-mdsip
loginctl enable-linger fdp-mdsip          # or the container dies with the session

# the trees must be readable by the new account, and only readable
setfacl -R -m u:fdp-mdsip:rX /srv/fdp/trees
```

Then install [`deploy/fdp-mdsip.container`](../deploy/fdp-mdsip.container) as a
systemd **user** unit under that account, and verify with the origin's uid
supplied so the check is live rather than skipped:

```bash
MDSIP_ORIGIN_UID=1122 pixi run sandbox-verify
```

The verifier reads `/proc/self/uid_map` through ordinary TDI and fails if
container-root maps to that uid. It is tested in both directions — it fails
against the current shared-account setup and passes against a separate one —
because a check that cannot fail proves nothing.

### One consequence: the two containers can no longer share a network

Rootless podman networks are **per-user**, so once mdsip runs as its own
account, the origin container cannot join `fdp-mdsip-net` and cannot resolve
`fdp-mdsip` by name. The relay has to reach mdsip over the host instead:
publish on `127.0.0.1:8000` as the unit does, and point the relay at
`host.containers.internal` rather than `localhost`.

```
http.exthandler mdsip /path/to/libXrdHttpMdsip.so prefix=/mdsip,host=host.containers.internal,port=8000
```

`localhost` inside the origin container is the *container*, not the host, so
leaving it there fails to connect with nothing obviously wrong in either log.
Confirm `host.containers.internal` resolves from inside the origin container
before relying on it; publishing on a host IP the origin can reach works too.

### What this does not fix

**The origin is one host.** mdsip and Pelican still share a kernel, so a kernel
escape still reaches everything on it regardless of uid. The separate account
raises the bar from "one namespace mapping away" to "needs a kernel bug *and*
a privilege escalation"; it does not remove the host as a shared fate. That is
the remaining argument for the microVM option below — a weaker one than before
this change, which is rather the point.

The relay can then be run against the sandbox rather than a bare mdsip — the
production topology, and the relay is indifferent to which:

```bash
MDSIP_SANDBOX=1 pixi run relay-e2e
```

## What is still open

**The kernel is the remaining boundary.** Everything above is namespaces,
cgroups and seccomp; a kernel exploit defeats all of it at once. `/dev/kvm` is
present on this host (`crw-rw-rw-`), so a microVM runtime — Kata, or Firecracker
via `crun --vm` — is available as a follow-on and would replace a shared kernel
with a virtualised one. It is the natural next hardening step, not a
prerequisite: the assets behind the current boundary are read-only public
archive data, and the crown jewels are already in a different container.

**Seccomp is podman's default profile**, not a tailored one. A profile derived
from what mdsip actually syscalls would shrink the kernel attack surface
considerably and is cheap to produce.

**Denial of service is bounded, not solved.** Limits cap one container, so a
client cannot take the host down, but it can degrade the service for everyone
else — there is no per-token quota or fair sharing, and adding one means the
relay tracking usage per session.

**The relay is what makes this reachable.** `libXrdHttpMdsip.so` deliberately
forwards anything a client sends, so the sandbox is the *only* control between a
token holder and code execution. Do not deploy the relay against an unsandboxed
mdsip, including "temporarily" for testing against production trees.
