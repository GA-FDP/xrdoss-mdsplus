# `libMdsIpFDP.so` — client transport design

**Status:** not built. The premise is verified and the blocker it depended on is
now cleared, but the work itself is a self-contained piece that deserves its own
TDD pass rather than being tacked onto the origin work.

## Goal

Existing DIII-D code that speaks only the MDSplus thin client should reach FDP
by changing **one string**:

```python
conn = MDSplus.Connection('fdp://osg-htc.org/fdp-d3d')   # was 'atlas.gat.com'
conn.openTree('efit01', 190000)
res = conn.getMany()...execute()
```

## What is already proven

**The loading seam needs no MDSplus patch.** `parse_host()`
(`mdstcpip/mdsipshr/ConnectToMds.c:41`) splits `<scheme>://<host>`, and
`LoadIo()` (`LoadIo.c:41-62`) uppercases the scheme, builds the image name
`"MdsIp" + SCHEME`, and resolves symbol `Io` via `LibFindImageSymbol_C`. So
`fdp://…` loads `libMdsIpFDP.so` exporting `IoRoutines *Io()`. Same
scheme-to-dlopen idiom as the `TreeFileIo` seam.

**The payload needs no translation.** `Connection.getMany()` sends
`GetManyExecute($)` with a serialized APD list — and that list is **byte-identical**
to what `tests/mkpath.py` puts in the object path. Verified: both produce the
same 135 bytes for `{'name':'ip','exp':'\\ipmhd'}`. The transport can lift arg 1
verbatim and base64url it into the path.

**Version discovery exists.** `GET /tdi-version/<tree>/<bucket>/<shot>` returns
the current token. `openTree()` is the natural place to call it: once per
tree-open, after which every expression in that session reuses it.

## What makes this bigger than it looks

`IoRoutines` is a **byte-stream** vtable —

```c
int     (*connect)(Connection*, char *protocol, char *connectString);
ssize_t (*send)(Connection*, const void *buf, size_t len, int nowait);
ssize_t (*recv)(Connection*, void *buf, size_t len);
int     (*disconnect)(Connection*);
```

— not a message-level one. To turn `getMany()` into an HTTP GET, the transport
must therefore **speak the mdsip protocol itself**: accumulate outgoing messages
(48-byte header plus payload, `nargs` of them) until a call is complete, decide
what was asked, and then synthesize a well-formed answer message on the way
back. That is the bulk of the work, and none of it is optional.

## Suggested shape

1. **Message framing** — encode/decode the 48-byte header. Pure logic, unit
   testable with no server, and the piece most likely to harbour a silent
   off-by-one. Do it first and fuzz it.
2. **Call recognition** — buffer args; recognise the two-arg call whose arg 0 is
   the C string `GetManyExecute($)`, and `MdsOpen` for `openTree`. Anything else
   returns a clear error rather than misbehaving: `put`, events and `setDefault`
   are out of scope (spec §1).
3. **HTTP** — libcurl GET, `Authorization: Bearer` from `BEARER_TOKEN` then
   `~/.fdp/token`, reusing `fdp`'s existing precedence rather than inventing one.
4. **Path building** — `BuildTdiPath` already exists in `src/TdiPath.cc` and is
   deliberately deterministic; link it rather than writing a fourth encoder.
5. **Answer synthesis** — wrap the fetched bytes in an answer message.

## Traps already known

- **Four implementations of the path grammar** will now exist: C++ plugin,
  `tests/mkpath.py`, the version resolver in each. They have already drifted
  once (`tests/fed/FINDINGS.md`). Build shared conformance vectors before adding
  a fourth.
- **`getMany` fans into one object, not N** (spec §9.3). A caller expecting one
  round trip per expression gets one per batch; document it.
- **Dimensions do not travel with values.** Callers wanting a time base add an
  explicit `dim_of()` item. This matches `atlas.gat.com`, so it is not a
  regression, but it will surprise anyone who assumed otherwise.
- **`ConnectToMds` returns 0 as a valid id**; only `-1` is failure.

## Why not a thinner client instead

A Python client that builds paths and fetches them would be perhaps a hundred
lines and would unblock `toksearch`'s `MdsSignal` (spec §10) immediately. It
does nothing for the existing thin-client code, which is the actual motivation,
but it is worth having and is a much smaller piece of work.
