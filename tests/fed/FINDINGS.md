# Task 0 findings — local Pelican federation harness

Run 2026-08-06 on omega06.gat.com. All three load-bearing premises of the design
were validated locally, with no access to the production origin.

## Environment

| Thing | Value |
|---|---|
| Image | `hub.opensciencegrid.org/pelican_platform/origin:latest` (868 MB, anonymously pullable) |
| Pelican | 7.26.0, build commit `4b5c65aa57f6f11f98305c1a333711f65c70b9b3` |
| **XRootD in image** | **v5.9.2** |
| XRootD in conda env | **v5.9.2** — identical, so the ABI concern for Task 1 Step 6 is resolved |
| podman | 4.9.4, rootless, graphroot already on `/local-scratch` (local disk, not NFS) |

## Premise 1 — `Xrootd.ConfigFile` + `ofs.osslib ++` works

Confirmed. The generated `/run/pelican/xrootd/origin/xrootd.cfg` ends with:

```
# Continue onto the next set of configuration
continue /etc/pelican/xrootd-extra.cfg
```

and the XRootD log shows a three-deep stack, our layer included:

```
ofs.osslib default
ofs.osslib ++ libXrdOssStats.so                 <- Pelican's own
ofs.osslib ++ /usr/lib64/libXrdOssStats-5.so    <- ours, via Xrootd.ConfigFile
Plugin loaded fsstats v5.9.2 from osslib /usr/lib64/libXrdOssStats-5.so
```

Pass-through survived: `/test/hello.txt` still returned `hello-federation`.

## Premise 2 — base64url survives the director

A **240-character** base64url segment came back from the director verbatim,
including the alphabet-specific `-` and `_`:

```
location: https://omega06.gat.com:8443/test/AAECAw...PD0-P0BB...fn-AgYKD...sbKz
```

and the object's bytes were served correctly.

Two controls prove the test discriminates:

| Control | Result |
|---|---|
| Segment of all `/` (standard base64 worst case) | **Collapsed entirely** — `location: .../test`, the whole segment vanished |
| `AAA%2FBBB` — can percent-encoding protect a slash? | **No** — became `AAA/BBB`, a real separator |

This is the empirical basis for spec §4.4's requirement that the alphabet
exclude `/` and `%`. Standard base64 would be catastrophically broken here, not
merely awkward.

## Premise 3 — bearer tokens do not fragment the cache key

`http.header2cgi Authorization authz` is present at line 27 of the generated
config. Effect on the director's redirect:

```
without token:  .../test/hello.txt
with token:     .../test/hello.txt?authz=dummy.token.value
```

The token lands in the query string, and XrdPfc keys on `URL.GetPath()`, which
splits at the first `?`. Path identical in both cases. Spec §4.2 confirmed.

## Corrections to the plan

1. **The registry module will not start without OIDC client files.** Contents are
   irrelevant; `fedbox.sh` mounts dummy `oidc-client-id` / `oidc-client-secret`.
   Without them the container exits with
   `failed to load server OIDC client config`.

2. **Pelican *does* emit an unconditional `ofs.osslib` for `posix`** —
   `ofs.osslib ++ libXrdOssStats.so`. The plan claimed it emitted none. This is
   good news rather than bad: it proves stacking is Pelican's own normal mode of
   operation, and our `++` simply adds a further layer.

3. **Never put `-5` in an `osslib` path.** XRootD appends the plugin version
   itself. Referencing `/usr/lib64/libXrdOssStats-5.so` produced:

   ```
   Config warning: osslib path '...-5.so' should not use '-5' version syntax in its name!
   Plugin osslib /usr/lib64/libXrdOssStats-5-5.so not found; falling back to ...
   ```

   It worked, but only via a fallback. **Our plugin must install as
   `libXrdOssMdsplus-5.so` and be referenced as `libXrdOssMdsplus.so`.** This
   affects `CMakeLists.txt` and every config fragment.

4. `oss.localroot` is `/run/pelican/xrootd/origin/export`, and each export
   produces an `all.export` line (`/test`, `/tdi`). Relevant to the LFN-prefix
   discovery in Task 9.

5. `ofs.authorize 1` and `ofs.authlib ++ libXrdAccSciTokens.so` are active even
   with `EnablePublicReads: true`.

## Task 1 addendum — ABI probe result

A conda-built plugin **loads cleanly into the image's XRootD**. The derived-image
fallback contemplated in the plan is not needed.

```
Plugin loaded XrdOssMdsplus v5.9.2 from osslib /plugins/libXrdOssMdsplus-5.so
++++++ XrdOssMdsplus scaffold loaded (pass-through only)
```

Both environments are XRootD v5.9.2, which is why this works — worth re-checking
if either side ever moves.

The naming convention from correction 3 above is confirmed in both directions:

| Config says | XRootD loads | Warning |
|---|---|---|
| `/plugins/libXrdOssMdsplus.so` (unsuffixed) | `/plugins/libXrdOssMdsplus-5.so` | **none** |
| `/usr/lib64/libXrdOssStats-5.so` (suffixed) | same file, via fallback | yes, `should not use '-5' version syntax` |

Resulting stack, with pass-through verified working:

```
ofs.osslib default
ofs.osslib ++ libXrdOssStats.so
ofs.osslib ++ /plugins/libXrdOssMdsplus.so
```

MDSplus `serialize()` / `deserialize()` also round-trips in this pixi
environment, including the `{name: {value|error}}` dictionary shape the
evaluator will return, so Task 4's core mechanism is confirmed available here.

## Reproduce

```bash
bash tests/fed/fedbox.sh start                    # plain federation
bash tests/fed/fedbox.sh start /tmp/extra.cfg     # with an osslib fragment
bash tests/fed/fedbox.sh start /tmp/extra.cfg /path/to/plugin.so
bash tests/fed/fedbox.sh stop
```
