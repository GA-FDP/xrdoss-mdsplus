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
find_path(MDSPLUS_INCLUDE_DIR NAMES mdsdescrip.h
    HINTS ${CMAKE_PREFIX_PATH}/include $ENV{PREFIX}/include $ENV{CONDA_PREFIX}/include)
find_path(MDSPLUS_LIB_DIR NAMES libMdsIpShr.so
    HINTS ${CMAKE_PREFIX_PATH}/lib $ENV{PREFIX}/lib $ENV{CONDA_PREFIX}/lib)
if(NOT MDSPLUS_INCLUDE_DIR OR NOT MDSPLUS_LIB_DIR)
    message(FATAL_ERROR "MdsIpShr not found (looking for mdsdescrip.h / libMdsIpShr.so)")
endif()
message(STATUS "MDSplus includes: ${MDSPLUS_INCLUDE_DIR}")
message(STATUS "MDSplus libs:     ${MDSPLUS_LIB_DIR}")
message(STATUS "XRootD includes: ${XROOTD_INCLUDE_DIR}")
message(STATUS "XRootD libs:     ${XROOTD_LIB_DIR}")

add_library(core OBJECT
    src/Base64Url.cc
    src/TdiPath.cc
    src/MdsIpClient.cc
    src/ResultCache.cc
)
target_include_directories(core PUBLIC src ${XROOTD_INCLUDE_DIR} ${MDSPLUS_INCLUDE_DIR})

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

include(GNUInstallDirs)
install(TARGETS XrdOssMdsplus LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    find_path(DOCTEST_INCLUDE_DIR NAMES doctest/doctest.h
        HINTS ${CMAKE_PREFIX_PATH}/include $ENV{PREFIX}/include $ENV{CONDA_PREFIX}/include)
    if(NOT DOCTEST_INCLUDE_DIR)
        message(FATAL_ERROR "doctest not found; add doctest to pixi.toml dependencies")
    endif()

    foreach(t base64url tdipath resultcache)
        add_executable(test_${t} tests/test_${t}.cpp $<TARGET_OBJECTS:core>)
        target_include_directories(test_${t} PRIVATE src ${XROOTD_INCLUDE_DIR}
                                   ${MDSPLUS_INCLUDE_DIR} ${DOCTEST_INCLUDE_DIR})
        target_link_directories(test_${t} PRIVATE ${MDSPLUS_LIB_DIR})
        target_link_libraries(test_${t} MdsIpShr)
        target_link_libraries(test_${t} pthread)
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()
endif()
