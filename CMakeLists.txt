cmake_minimum_required(VERSION 3.16)
project(XrdOssMdsplus VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The three plugins are independently buildable, because their dependencies do
# not overlap and the conda packages built from them are separate: the client
# transport needs MDSplus and curl but no XRootD, and the relay needs XRootD but
# -- as its whole design point -- no MDSplus at all. Building one must therefore
# not require the other's dependencies to be installed.
option(BUILD_CLIENT "Build the MDSplus fdp:// transport (libMdsIpFDP.so)" ON)
option(BUILD_RELAY  "Build the mdsip relay (libXrdHttpMdsip-5.so)" ON)
option(BUILD_OSS    "Build the virtual-file plugin (libXrdOssMdsplus-5.so)" ON)
option(BUILD_TESTS  "Build tests" ON)

# Tests span all three, so they pull in every dependency regardless of which
# plugins are selected. A packaging build turns them off.
if(BUILD_OSS OR BUILD_RELAY OR BUILD_TESTS)
    set(NEED_XROOTD ON)
endif()
if(BUILD_OSS OR BUILD_CLIENT OR BUILD_TESTS)
    set(NEED_MDSPLUS ON)
endif()
if(BUILD_CLIENT OR BUILD_TESTS)
    set(NEED_CURL ON)
endif()

if(NEED_XROOTD)
    # Conda does not ship .pc files for XRootD, so find it by hand.
    find_path(XROOTD_INCLUDE_DIR
        NAMES XrdOss/XrdOssWrapper.hh
        HINTS ${CMAKE_PREFIX_PATH}/include/xrootd $ENV{PREFIX}/include/xrootd $ENV{CONDA_PREFIX}/include/xrootd
    )
    find_path(XROOTD_LIB_DIR
        NAMES libXrdUtils.so
        HINTS ${CMAKE_PREFIX_PATH}/lib $ENV{PREFIX}/lib $ENV{CONDA_PREFIX}/lib
    )
    if(NOT XROOTD_INCLUDE_DIR)
        message(FATAL_ERROR "XRootD headers not found (looking for XrdOss/XrdOssWrapper.hh)")
    endif()
    if(NOT XROOTD_LIB_DIR)
        message(FATAL_ERROR "XRootD libraries not found")
    endif()
    message(STATUS "XRootD includes: ${XROOTD_INCLUDE_DIR}")
    message(STATUS "XRootD libs:     ${XROOTD_LIB_DIR}")
endif()

# MDSplus: only MdsIpShr, the mdsip protocol client. No object model, no
# treeshr -- nothing that opens files or evaluates TDI in this process.
# Headers are vendored: the MDSplus binary RPMs ship none. See
# third_party/mdsplus/README.md.
set(MDSPLUS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mdsplus)
if(NEED_MDSPLUS)
    find_path(MDSPLUS_LIB_DIR NAMES libMdsIpShr.so
        HINTS ${CMAKE_PREFIX_PATH}/lib $ENV{PREFIX}/lib $ENV{CONDA_PREFIX}/lib)
    if(NOT MDSPLUS_LIB_DIR)
        message(FATAL_ERROR "libMdsIpShr.so not found")
    endif()
    message(STATUS "MDSplus includes: ${MDSPLUS_INCLUDE_DIR}")
    message(STATUS "MDSplus libs:     ${MDSPLUS_LIB_DIR}")
endif()

# XrdHttpExtHandler.hh ships in neither xrootd-devel nor conda-forge's xrootd;
# it exists only in the XRootD source tree. See third_party/xrootd/README.md.
set(XROOTD_VENDORED_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xrootd)
if(BUILD_RELAY)
    find_library(XROOTD_HTTPUTILS_LIB NAMES XrdHttpUtils HINTS ${XROOTD_LIB_DIR})
    if(NOT XROOTD_HTTPUTILS_LIB)
        message(FATAL_ERROR "libXrdHttpUtils.so not found; it implements XrdHttpExtReq")
    endif()
endif()

if(NEED_CURL)
    find_package(CURL REQUIRED)
endif()

if(BUILD_OSS OR BUILD_TESTS)
    add_library(core OBJECT
        src/Base64Url.cc
        src/TdiPath.cc
        src/TreeVersion.cc
        src/MdsIpClient.cc
        src/ResultCache.cc
    )
    target_include_directories(core PUBLIC src ${XROOTD_INCLUDE_DIR} ${MDSPLUS_INCLUDE_DIR})
endif()

# The relay shares nothing with `core`: it links no MDSplus library at all,
# speaking mdsip over a plain socket rather than through libMdsIpShr.
if(BUILD_RELAY OR BUILD_TESTS)
    add_library(relaycore OBJECT src/MdsipSession.cc)
    target_include_directories(relaycore PUBLIC src)
endif()

# Client transport: the tunnel's other half.
if(BUILD_CLIENT)
    add_library(clientcore OBJECT src/MdsipCall.cc src/HttpTunnel.cc)
    target_include_directories(clientcore PUBLIC src ${MDSPLUS_INCLUDE_DIR})
    target_link_libraries(clientcore PUBLIC CURL::libcurl)
endif()

if(BUILD_OSS)
    add_library(XrdOssMdsplus MODULE
        src/OssMdsplus.cc
        $<TARGET_OBJECTS:core>
    )
    target_include_directories(XrdOssMdsplus PRIVATE src ${XROOTD_INCLUDE_DIR})
    target_link_directories(XrdOssMdsplus PRIVATE ${XROOTD_LIB_DIR})
    target_link_directories(XrdOssMdsplus PRIVATE ${MDSPLUS_LIB_DIR})
    target_link_libraries(XrdOssMdsplus XrdUtils MdsIpShr)

    # XRootD appends the plugin version to an osslib name itself, so the file on
    # disk must carry -5 while configs reference the UNSUFFIXED name. Referencing
    # "...-5.so" makes XRootD look for "...-5-5.so" and only reach the right file
    # via a fallback (observed in Task 0).
    set_target_properties(XrdOssMdsplus PROPERTIES PREFIX "lib" OUTPUT_NAME "XrdOssMdsplus-5")
    list(APPEND INSTALL_TARGETS XrdOssMdsplus)
endif()

# The mdsip-over-HTTPS relay: an XrdHttpExtHandler on the origin's own port.
if(BUILD_RELAY)
    add_library(XrdHttpMdsip MODULE
        src/HttpRelay.cc
        $<TARGET_OBJECTS:relaycore>
    )
    target_include_directories(XrdHttpMdsip PRIVATE src ${XROOTD_INCLUDE_DIR}
                               ${XROOTD_VENDORED_INCLUDE_DIR})
    target_link_directories(XrdHttpMdsip PRIVATE ${XROOTD_LIB_DIR})
    target_link_libraries(XrdHttpMdsip XrdUtils ${XROOTD_HTTPUTILS_LIB} pthread)
    # Same -5 suffix rule as the osslib above: XrdOucPinLoader appends the plugin
    # version, so configs must reference the UNSUFFIXED name.
    set_target_properties(XrdHttpMdsip PROPERTIES PREFIX "lib" OUTPUT_NAME "XrdHttpMdsip-5")
    list(APPEND INSTALL_TARGETS XrdHttpMdsip)
endif()

# The MDSplus thin-client transport. MDSplus's LoadIo() builds the image name
# "MdsIp" + uppercased scheme and resolves the symbol Io, so scheme fdp:// means
# this file must be named libMdsIpFDP.so EXACTLY -- no version suffix. That is
# the opposite of the XRootD plugins above, where XrdOucPinLoader appends one.
if(BUILD_CLIENT)
    add_library(MdsIpFDP MODULE
        src/MdsIpFDP.cc
        $<TARGET_OBJECTS:clientcore>
    )
    target_include_directories(MdsIpFDP PRIVATE src ${MDSPLUS_INCLUDE_DIR})
    target_link_directories(MdsIpFDP PRIVATE ${MDSPLUS_LIB_DIR})
    # MdsIpShr provides ConnectionSetInfo/ConnectionGetInfo. It is always already
    # loaded in the process -- LoadIo() lives in it -- but linking makes the
    # dependency explicit rather than relying on the loader's global scope.
    target_link_libraries(MdsIpFDP CURL::libcurl MdsIpShr)
    set_target_properties(MdsIpFDP PROPERTIES PREFIX "lib" OUTPUT_NAME "MdsIpFDP")
    list(APPEND INSTALL_TARGETS MdsIpFDP)
endif()

include(GNUInstallDirs)
if(INSTALL_TARGETS)
    install(TARGETS ${INSTALL_TARGETS} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

# The modern PTDATA2/PTHEAD2/PTHEAD2_ASCII, shipped with the CLIENT package.
# They are MDSplus-side TDI, so they belong with mdsip-fdp; the relay links no
# MDSplus at all and has no use for them.
#
# Installed to share/fdp/tdi rather than under <prefix>/tdi DELIBERATELY.
# MDS_PATH entries are searched in order AND recursively, so putting these
# under a directory a consumer already has on MDS_PATH would make resolution
# depend on which of two same-named .fun files the recursive walk reaches
# first. Measured 2026-08-17: with both a fdp/ and a d3d/ptdata2/ copy under
# one entry, ours won -- but that is unspecified behaviour, not a guarantee,
# and if it ever flipped the caller would silently get the SITE version, which
# is the one with the `missing || operators` bug. Install outside, and make the
# consumer name this directory explicitly and ahead of the site library:
#
#     MDS_PATH="$CONDA_PREFIX/share/fdp/tdi;$CONDA_PREFIX/tdi"
#
# Explicit and ordered beats lucky.
if(BUILD_CLIENT)
    file(GLOB FDP_TDI_FUNS "${CMAKE_CURRENT_SOURCE_DIR}/tdi/fdp/*.fun")
    if(NOT FDP_TDI_FUNS)
        message(FATAL_ERROR
            "tdi/fdp/*.fun not found -- the client package would install no TDI "
            "functions and PTDATA2 would resolve to the site version.")
    endif()
    install(FILES ${FDP_TDI_FUNS}
            DESTINATION ${CMAKE_INSTALL_DATADIR}/fdp/tdi)
endif()

# libptd3d, for the HTTP point endpoint. Built PTDATA_WITH_FDPIO=OFF and
# PTDATA_WITH_HTTP=OFF so the library is physically incapable of remote I/O:
# the origin reads local files, and a library that cannot reach the network
# cannot be turned into an egress path.
#
# Optional on purpose. The relay predates this and must keep building where
# libptd3d is not installed; the point endpoint simply is not built there.
set(PTDATA_ROOT "" CACHE PATH "libptd3d install prefix")
find_path(PTDATA_INCLUDE_DIR NAMES ptdata/shot_locator.h
    HINTS ${PTDATA_ROOT}/include/ptd3d $ENV{PTDATA_ROOT}/include/ptd3d
          ${CMAKE_PREFIX_PATH}/include/ptd3d)
find_library(PTDATA_LIBRARY NAMES ptd3d
    HINTS ${PTDATA_ROOT}/lib ${PTDATA_ROOT}/lib64 $ENV{PTDATA_ROOT}/lib
          ${CMAKE_PREFIX_PATH}/lib)

if(PTDATA_INCLUDE_DIR AND PTDATA_LIBRARY)
    set(HAVE_PTDATA TRUE)
    message(STATUS "Found libptd3d: ${PTDATA_LIBRARY}")
    add_library(pointcore OBJECT src/ArchiveIo.cc src/PointStore.cc)
    target_include_directories(pointcore PRIVATE src ${PTDATA_INCLUDE_DIR})
    # ptdata's headers need C++20 (std::span, std::optional). Raised only on
    # these targets: the rest of the repo is C++14 for XRootD and MDSplus, and
    # PointStore.hh deliberately exposes no ptdata type so nothing else has to
    # move.
    target_compile_features(pointcore PUBLIC cxx_std_20)
else()
    set(HAVE_PTDATA FALSE)
    message(STATUS "libptd3d not found; the HTTP point endpoint will not be built")
endif()

if(BUILD_TESTS)
    enable_testing()
    find_path(DOCTEST_INCLUDE_DIR NAMES doctest/doctest.h
        HINTS ${CMAKE_PREFIX_PATH}/include $ENV{PREFIX}/include $ENV{CONDA_PREFIX}/include)
    if(NOT DOCTEST_INCLUDE_DIR)
        message(FATAL_ERROR "doctest not found; add doctest to pixi.toml dependencies")
    endif()

    if(HAVE_PTDATA)
        add_executable(test_archiveio tests/test_archiveio.cpp $<TARGET_OBJECTS:pointcore>)
        target_include_directories(test_archiveio PRIVATE src ${PTDATA_INCLUDE_DIR}
                                   ${DOCTEST_INCLUDE_DIR})
        target_link_libraries(test_archiveio ${PTDATA_LIBRARY} pthread)
        target_compile_features(test_archiveio PRIVATE cxx_std_20)
        add_test(NAME archiveio COMMAND test_archiveio)

        add_executable(test_pointstore tests/test_pointstore.cpp $<TARGET_OBJECTS:pointcore>)
        target_include_directories(test_pointstore PRIVATE src ${PTDATA_INCLUDE_DIR}
                                   ${DOCTEST_INCLUDE_DIR})
        target_link_libraries(test_pointstore ${PTDATA_LIBRARY} pthread)
        target_compile_features(test_pointstore PRIVATE cxx_std_20)
        add_test(NAME pointstore COMMAND test_pointstore)
    endif()

    foreach(t base64url tdipath treeversion resultcache)
        add_executable(test_${t} tests/test_${t}.cpp $<TARGET_OBJECTS:core>)
        target_include_directories(test_${t} PRIVATE src ${XROOTD_INCLUDE_DIR}
                                   ${MDSPLUS_INCLUDE_DIR} ${DOCTEST_INCLUDE_DIR})
        target_link_directories(test_${t} PRIVATE ${MDSPLUS_LIB_DIR})
        target_link_libraries(test_${t} MdsIpShr)
        target_link_libraries(test_${t} pthread)
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()

    # Links relaycore, not core: no MDSplus anywhere in the relay path.
    add_executable(test_mdsipsession tests/test_mdsipsession.cpp
                   $<TARGET_OBJECTS:relaycore>)
    target_include_directories(test_mdsipsession PRIVATE src ${DOCTEST_INCLUDE_DIR})
    target_link_libraries(test_mdsipsession pthread)
    add_test(NAME mdsipsession COMMAND test_mdsipsession)

    # Call framing is the client transport's riskiest logic -- it turns a byte
    # stream with arbitrary chunking into whole calls -- and needs no server.
    add_executable(test_mdsipcall tests/test_mdsipcall.cpp src/MdsipCall.cc)
    target_include_directories(test_mdsipcall PRIVATE src ${DOCTEST_INCLUDE_DIR})
    add_test(NAME mdsipcall COMMAND test_mdsipcall)

    add_executable(test_httptunnel tests/test_httptunnel.cpp src/HttpTunnel.cc)
    target_include_directories(test_httptunnel PRIVATE src ${DOCTEST_INCLUDE_DIR})
    target_link_libraries(test_httptunnel CURL::libcurl)
    add_test(NAME httptunnel COMMAND test_httptunnel)
endif()
