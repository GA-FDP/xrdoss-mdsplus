#!/usr/bin/env bash
# libXrdHttpMdsip-5.so -- the mdsip-over-HTTPS relay.
set -euo pipefail

# BUILD_CLIENT/BUILD_OSS off so this build needs no MDSplus. That is not a
# convenience: the relay linking an MDSplus library would contradict its design
# and would break on an origin that has none. The package test checks the
# resulting binary for it.
cmake -S "$SRC_DIR" -B build-relay \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DBUILD_TESTS=OFF \
    -DBUILD_CLIENT=OFF \
    -DBUILD_OSS=OFF

cmake --build build-relay -j"${CPU_COUNT:-2}"
cmake --install build-relay
