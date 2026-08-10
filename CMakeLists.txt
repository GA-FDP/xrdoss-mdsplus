cmake_minimum_required(VERSION 3.16)
project(XrdOssMdsplus VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
# MDSplus: only MdsIpShr, the mdsip protocol client. No object model, no
# treeshr -- nothing that opens files or evaluates TDI in this process.
# Headers are vendored: the MDSplus binary RPMs ship none. See
# third_party/mdsplus/README.md.
set(MDSPLUS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mdsplus)
find_path(MDSPLUS_LIB_DIR NAMES libMdsIpShr.so
    HINTS ${CMAKE_PREFIX_PATH}/lib $ENV{PREFIX}/lib $ENV{CONDA_PREFIX}/lib)
if(NOT MDSPLUS_LIB_DIR)
    message(FATAL_ERROR "libMdsIpShr.so not found")
endif()
message(STATUS "MDSplus includes: ${MDSPLUS_INCLUDE_DIR}")
message(STATUS "MDSplus libs:     ${MDSPLUS_LIB_DIR}")
message(STATUS "XRootD includes: ${XROOTD_INCLUDE_DIR}")
message(STATUS "XRootD libs:     ${XROOTD_LIB_DIR}")

# XrdHttpExtHandler.hh ships in neither xrootd-devel nor conda-forge's xrootd;
# it exists only in the XRootD source tree. See third_party/xrootd/README.md.
set(XROOTD_VENDORED_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xrootd)
find_library(XROOTD_HTTPUTILS_LIB NAMES XrdHttpUtils HINTS ${XROOTD_LIB_DIR})
if(NOT XROOTD_HTTPUTILS_LIB)
    message(FATAL_ERROR "libXrdHttpUtils.so not found; it implements XrdHttpExtReq")
endif()

add_library(core OBJECT
    src/Base64Url.cc
    src/TdiPath.cc
    src/TreeVersion.cc
    src/MdsIpClient.cc
    src/ResultCache.cc
)
target_include_directories(core PUBLIC src ${XROOTD_INCLUDE_DIR} ${MDSPLUS_INCLUDE_DIR})

# The relay shares nothing with `core`: it links no MDSplus library at all,
# speaking mdsip over a plain socket rather than through libMdsIpShr.
add_library(relaycore OBJECT src/MdsipSession.cc)
target_include_directories(relaycore PUBLIC src)

# Client transport: the tunnel's other half.
find_package(CURL REQUIRED)
add_library(clientcore OBJECT src/MdsipCall.cc src/HttpTunnel.cc)
target_include_directories(clientcore PUBLIC src ${MDSPLUS_INCLUDE_DIR})
target_link_libraries(clientcore PUBLIC CURL::libcurl)

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

# The mdsip-over-HTTPS relay: an XrdHttpExtHandler on the origin's own port.
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

# The MDSplus thin-client transport. MDSplus's LoadIo() builds the image name
# "MdsIp" + uppercased scheme and resolves the symbol Io, so scheme fdp:// means
# this file must be named libMdsIpFDP.so EXACTLY -- no version suffix. That is
# the opposite of the XRootD plugins above, where XrdOucPinLoader appends one.
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

include(GNUInstallDirs)
install(TARGETS XrdOssMdsplus XrdHttpMdsip MdsIpFDP
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    find_path(DOCTEST_INCLUDE_DIR NAMES doctest/doctest.h
        HINTS ${CMAKE_PREFIX_PATH}/include $ENV{PREFIX}/include $ENV{CONDA_PREFIX}/include)
    if(NOT DOCTEST_INCLUDE_DIR)
        message(FATAL_ERROR "doctest not found; add doctest to pixi.toml dependencies")
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
