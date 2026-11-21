# Separate CMake for LLVM codegen so we can tweak C++ standard / warnings independently.

cmake_minimum_required(VERSION 3.20)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "LLVM codegen enabled (LLVM ${LLVM_PACKAGE_VERSION})")

if(APPLE)
    foreach(dep IN ITEMS ZLIB::ZLIB LibEdit::LibEdit zstd::libzstd_shared zstd::libzstd_static)
        if(TARGET ${dep})
            set_target_properties(${dep} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "")
        endif()
    endforeach()
endif()

# Require LLVM 20 or newer. Some APIs used in this target rely on LLVM 20+.
# Prefer the provided major variable; if unavailable, derive it from LLVM_PACKAGE_VERSION.
if (NOT DEFINED LLVM_VERSION_MAJOR)
    string(REGEX MATCH "^[0-9]+" LLVM_VERSION_MAJOR "${LLVM_PACKAGE_VERSION}")
endif()

if (LLVM_VERSION_MAJOR LESS 20)
    message(FATAL_ERROR "LLVM 20 or newer is required for oz_codegen_llvm. Found ${LLVM_PACKAGE_VERSION}.")
endif()

# We purposely use at least C++20 here (could diverge from core lib which uses cxx_std_23 already).
add_library(qumir_codegen_llvm
    llvm_asm_printer.cpp
    llvm_asm_printer.h
    llvm_backend.cpp
    llvm_backend.h
    llvm_initializer.cpp
    llvm_initializer.h
    llvm_codegen.cpp
    llvm_codegen.h
    llvm_debug_info.cpp
    llvm_debug_info.h
    llvm_runner.cpp
    llvm_runner.h
    symbol_object_cache.cpp
    symbol_object_cache.h
)

target_include_directories(qumir_codegen_llvm
    PRIVATE
        ${LLVM_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../.. # project root include
)

# C++23 for std::expected (matches the core lib).
target_compile_features(qumir_codegen_llvm PUBLIC cxx_std_23)

# Propagate minimal definitions
add_definitions(${LLVM_DEFINITIONS})

# Prefer component mapping so we stay portable across LLVM builds.
set(LLVM_COMPONENTS
    analysis
    scalaropts
    transformutils
    ipo
    vectorize
    instcombine
    passes
    core
    bitreader
    bitwriter
    linker
    support
    mc
    mcparser
    orcjit
    jitlink
    executionengine
    target
    asmparser
    asmprinter
    native          # host target (dispatcher + minimal codegen)
    nativecodegen   # full codegen for host (brings in actual target libs)

    WebAssemblyCodeGen
    WebAssemblyDesc
    WebAssemblyInfo
    WebAssemblyAsmParser
    WebAssemblyDisassembler
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND LLVM_COMPONENTS orcdebugging orctargetprocess)
endif()

if(QUMIR_LLVM_LINK_SHARED)
    if(NOT TARGET LLVM)
        message(FATAL_ERROR
            "QUMIR_LLVM_LINK_SHARED=ON, but this LLVM installation ships no shared "
            "libLLVM (check llvm-config --shared-mode)")
    endif()
    # The static component archives add ~50 MB of LLVM to every executable that
    # links the codegen; the shared library keeps one copy per system instead.
    target_link_libraries(qumir_codegen_llvm PRIVATE LLVM)
else()
    llvm_map_components_to_libnames(LLVM_LIBS ${LLVM_COMPONENTS})
    target_link_libraries(qumir_codegen_llvm PRIVATE ${LLVM_LIBS})
endif()

if(UNIX AND NOT APPLE)
    target_link_options(qumir_codegen_llvm PUBLIC -rdynamic)
endif()
target_link_libraries(qumir_codegen_llvm PUBLIC qumir_runtime_future)
