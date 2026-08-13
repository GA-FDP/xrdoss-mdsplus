#!/usr/bin/env bash
# libMdsIpFDP.so -- the MDSplus fdp:// transport.
set -euo pipefail

# BUILD_RELAY/BUILD_OSS off so this build needs no XRootD at all: the client
# package's host environment does not contain it, and asking CMake to look
# would fail configure rather than skip a target.
#
# CMAKE_INSTALL_LIBDIR=lib because GNUInstallDirs picks lib64 on a 64-bit host,
# and conda puts everything in lib. The transport in particular MUST land next
# to libMdsShr.so: that library's RPATH is $ORIGIN/., which is how MDSplus finds
# this one with no LD_LIBRARY_PATH set.
cmake -S "$SRC_DIR" -B build-client \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DBUILD_TESTS=OFF \
    -DBUILD_RELAY=OFF \
    -DBUILD_OSS=OFF

cmake --build build-client -j"${CPU_COUNT:-2}"
cmake --install build-client
