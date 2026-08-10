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
| Read-only root | `--read-only` | persistence, replacing the server binary |
| Read-only trees | `-v $TREES:/trees:ro` | tampering with archive data |
| Writable scratch that cannot execute | `--tmpfs /tmp:noexec,nosuid,nodev` | staging a binary |
| No capabilities | `--cap-drop=ALL` | most escape paths |
| No privilege regain | `--security-opt=no-new-privileges` | setuid escalation |
| Not uid 0 | `USER mdsip` (5000) | shortening the path out of the namespace |
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
is the fork-bomb control quietly absent. `mdsip-sandbox.sh` now **refuses to
start** on such a host unless `MDSIP_ALLOW_NO_LIMITS=1` says the risk is
accepted deliberately, and the verifier reports those controls as WAIVED rather
than passing over them.

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
