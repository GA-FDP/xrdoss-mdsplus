/* Deliberately empty.
 *
 * Upstream mdsdescrip.h includes <mdsplus/mdsconfig.h>, which MDSplus generates
 * at build time by autoconf. It is a build artifact, not a source file, so it
 * is not vendorable -- and nothing the descriptor definitions need actually
 * comes from it. Verified: the six vendored headers compile against this empty
 * stub and yield the same struct layout as a full MDSplus build
 * (sizeof(descriptor_a) == 32, offsetof(arsize) == 28).
 *
 * If a future MDSplus starts requiring real content here, compilation breaks
 * loudly rather than silently changing the ABI.
 */
