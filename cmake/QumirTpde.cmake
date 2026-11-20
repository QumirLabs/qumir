# Provides the tpde and tpde_encodegen targets for QUMIR_ENABLE_TPDE.
# Order: targets from a parent project, QUMIR_TPDE_SOURCE_DIR, then FetchContent.

set(QUMIR_TPDE_SOURCE_DIR "" CACHE PATH "Use this TPDE checkout instead of fetching one")

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "QUMIR_ENABLE_TPDE: TPDE emits ELF objects and supports Linux only, "
        "found ${CMAKE_SYSTEM_NAME}")
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|aarch64|arm64)$")
    message(FATAL_ERROR "QUMIR_ENABLE_TPDE: TPDE supports x86-64 and AArch64 only, "
        "found ${CMAKE_SYSTEM_PROCESSOR}")
endif()

find_package(LLVM CONFIG)
if(NOT LLVM_FOUND)
    message(FATAL_ERROR "QUMIR_ENABLE_TPDE: LLVM 22.1 or 23.1 is required to build tpde_encodegen")
endif()
set(_qumir_llvm_version "${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}")
if(NOT _qumir_llvm_version STREQUAL "22.1" AND NOT _qumir_llvm_version STREQUAL "23.1")
    message(FATAL_ERROR "QUMIR_ENABLE_TPDE: TPDE supports LLVM 22.1 and 23.1, "
        "found ${LLVM_VERSION} in ${LLVM_DIR} (point LLVM_DIR at a supported one)")
endif()

if(TARGET tpde AND TARGET tpde_encodegen)
    message(STATUS "TPDE: using the targets provided by the parent project")
    return()
endif()

set(_qumir_tpde_src "${QUMIR_TPDE_SOURCE_DIR}")
if(NOT _qumir_tpde_src)
    set(_qumir_tpde_src "${FETCHCONTENT_SOURCE_DIR_TPDE}")
endif()
if(_qumir_tpde_src AND NOT EXISTS "${_qumir_tpde_src}/deps/fadec/CMakeLists.txt")
    message(FATAL_ERROR "QUMIR_ENABLE_TPDE: ${_qumir_tpde_src} has no deps/fadec, "
        "run 'git submodule update --init deps/fadec deps/disarm' there")
endif()

# Only the library and the encoder generator: no tests, no spdlog.
set(TPDE_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
set(TPDE_ENABLE_LLVM OFF CACHE BOOL "" FORCE)
set(TPDE_ENABLE_ENCODEGEN ON CACHE BOOL "" FORCE)
set(TPDE_LOGGING OFF CACHE STRING "" FORCE)

include(FetchContent)
if(QUMIR_TPDE_SOURCE_DIR)
    FetchContent_Declare(tpde SOURCE_DIR "${QUMIR_TPDE_SOURCE_DIR}" EXCLUDE_FROM_ALL)
else()
    # Of the TPDE submodules only fadec and disarm are linked into the library.
    FetchContent_Declare(tpde
        GIT_REPOSITORY https://github.com/tpde2/tpde.git
        GIT_TAG 050927ee81dc11a9e599fa2524ff0eef16daf51a
        GIT_SUBMODULES deps/fadec deps/disarm
        EXCLUDE_FROM_ALL)
endif()
FetchContent_MakeAvailable(tpde)
message(STATUS "TPDE: built from ${tpde_SOURCE_DIR}")
