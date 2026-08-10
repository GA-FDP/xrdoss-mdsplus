# Vendored MDSplus headers

MDSplus's RHEL binary RPMs ship **no headers at all** — `mdsplus-devel_bin`
contains 19 files, every one a `.a` archive. There is no header-providing
package, so building against the RPM's static libraries requires the headers
from somewhere else.

These six are the minimal closed set needed to compile `src/MdsIpClient.cc`:

| File | Why |
|---|---|
| `mdsdescrip.h` | `descriptor_a`, `descriptor_xd`, `EMPTYXD` |
| `status.h`, `mdsshr_messages.h` | included by `mdsdescrip.h` |
| `dtypedef.h`, `classdef.h`, `opcbuiltins.h` | `DTYPE_*`, `CLASS_*` |
| `mdsplus/mdsconfig.h` | **empty stub** — see the comment in the file |

**Provenance:** MDSplus source, `GA-FDP/mdsplus-xrdcl` at
`release-1.0.1-11-g5bacf045c`, whose `include/` is unmodified upstream.
Licensed BSD 3-clause (MIT/Massachusetts Institute of Technology); see
`LICENSE`.

**To refresh:** copy the six files from a MDSplus source checkout's `include/`
directory and keep `mdsplus/mdsconfig.h` empty. The build fails loudly if the
set stops closing.

These replaced a hand-written `src/MdsDescriptor.hh` that declared the two
structs and asserted their offsets. That worked — its asserts caught a real
transcription error — but using upstream's own definitions removes the
opportunity to be subtly wrong.
