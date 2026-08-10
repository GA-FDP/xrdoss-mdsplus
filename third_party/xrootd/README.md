# Vendored XRootD header

`XrdHttp/XrdHttpExtHandler.hh` is the interface for an HTTP extension handler —
the mechanism Pelican itself uses twice (`libXrdHttpPelican.so`,
`libXrdHttpTPC.so`).

It is **not installed** by `xrootd-devel` or by conda-forge's `xrootd`; it exists
only in the XRootD source tree. Its only dependency, `XrdNet/XrdNetPMark.hh`,
*is* installed in both, so this single file is the whole vendoring cost.

**Provenance:** XRootD v5.9.2 source (`src/XrdHttp/XrdHttpExtHandler.hh`),
matching the version pinned in `pixi.toml` and shipped in the Pelican origin
image. LGPL-3.0-or-later, as XRootD.

Being uninstalled suggests upstream treats this as less stable than the `XrdOss`
interface. Re-check it when moving XRootD versions; the build fails loudly if
the signatures move.
