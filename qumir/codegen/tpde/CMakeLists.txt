# Experimental TPDE backend: lowers Qumir IR directly to machine code.
# LLVM is needed only at build time (clang + tpde_encodegen turn snippets.c
# into instruction encoders); the generated code path never calls LLVM.

find_package(LLVM REQUIRED CONFIG)
include(FetchContent)

set(TPDE_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
set(TPDE_ENABLE_LLVM OFF CACHE BOOL "" FORCE)
set(TPDE_ENABLE_ENCODEGEN ON CACHE BOOL "" FORCE)
set(TPDE_LOGGING "DebugOnly" CACHE STRING "" FORCE)

FetchContent_Declare(tpde
    GIT_REPOSITORY https://github.com/tpde2/tpde.git
    GIT_TAG 050927ee81dc11a9e599fa2524ff0eef16daf51a
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(tpde)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(QUMIR_TPDE_ARCH a64)
    set(QUMIR_TPDE_CLANG_TARGET --target=aarch64-linux -march=armv8.1-a)
else()
    message(FATAL_ERROR "QUMIR_ENABLE_TPDE: only AArch64 Linux is implemented so far")
endif()

find_program(QUMIR_TPDE_CLANG NAMES clang-${LLVM_VERSION_MAJOR} clang
    HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)

set(SNIPPETS_BC ${CMAKE_CURRENT_BINARY_DIR}/snippets_${QUMIR_TPDE_ARCH}.bc)
set(SNIPPETS_HPP ${CMAKE_CURRENT_BINARY_DIR}/snippets_${QUMIR_TPDE_ARCH}.hpp)
add_custom_command(
    OUTPUT ${SNIPPETS_BC}
    COMMAND ${QUMIR_TPDE_CLANG} -c -emit-llvm -ffreestanding -fcf-protection=none
        -O3 -fomit-frame-pointer -fno-math-errno ${QUMIR_TPDE_CLANG_TARGET}
        -o ${SNIPPETS_BC} ${CMAKE_CURRENT_SOURCE_DIR}/snippets.c
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/snippets.c)
add_custom_command(
    OUTPUT ${SNIPPETS_HPP}
    COMMAND tpde_encodegen -o ${SNIPPETS_HPP} ${SNIPPETS_BC}
        ${CMAKE_CURRENT_SOURCE_DIR}/snippets_div.ll
    DEPENDS tpde_encodegen ${SNIPPETS_BC} ${CMAKE_CURRENT_SOURCE_DIR}/snippets_div.ll)

add_library(qumir_codegen_tpde
    tpde_codegen.h
    tpde_adaptor.h
    tpde_compiler_${QUMIR_TPDE_ARCH}.cpp
    ${SNIPPETS_HPP})
target_include_directories(qumir_codegen_tpde PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
target_compile_features(qumir_codegen_tpde PUBLIC cxx_std_23)
# Uses qumir IR symbols; they are linked through qumir, which links this library.
target_link_libraries(qumir_codegen_tpde PRIVATE tpde)
target_compile_definitions(qumir_codegen_tpde PUBLIC QUMIR_ENABLE_TPDE)

option(QUMIR_TPDE_ASSERTIONS "Enable TPDE header assertions in the Qumir adaptor" OFF)
if(QUMIR_TPDE_ASSERTIONS)
    target_compile_options(qumir_codegen_tpde PRIVATE -UNDEBUG)
endif()
