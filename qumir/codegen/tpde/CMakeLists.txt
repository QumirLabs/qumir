# LLVM is needed at build time only: clang and tpde_encodegen turn the
# snippets into instruction encoders.

find_program(QUMIR_TPDE_CLANG NAMES clang-${LLVM_VERSION_MAJOR} clang
    HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)

# The platform follows the build host: the snippets are compiled for it and
# tpde_compiler_<arch>.cpp is built.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(QUMIR_TPDE_ARCH x64)
    set(QUMIR_TPDE_SNIPPETS_TARGET --target=x86_64-linux -march=x86-64)
else()
    set(QUMIR_TPDE_ARCH a64)
    set(QUMIR_TPDE_SNIPPETS_TARGET --target=aarch64-linux -march=armv8.1-a)
endif()

set(SNIPPETS_BC ${CMAKE_CURRENT_BINARY_DIR}/snippets_${QUMIR_TPDE_ARCH}.bc)
set(SNIPPETS_HPP ${CMAKE_CURRENT_BINARY_DIR}/snippets_${QUMIR_TPDE_ARCH}.hpp)
add_custom_command(
    OUTPUT ${SNIPPETS_BC}
    COMMAND ${QUMIR_TPDE_CLANG} -c -emit-llvm -ffreestanding -fcf-protection=none
        -O3 -fomit-frame-pointer -fno-math-errno ${QUMIR_TPDE_SNIPPETS_TARGET}
        -o ${SNIPPETS_BC} ${CMAKE_CURRENT_SOURCE_DIR}/snippets.c
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/snippets.c)
add_custom_command(
    OUTPUT ${SNIPPETS_HPP}
    COMMAND tpde_encodegen -o ${SNIPPETS_HPP} ${SNIPPETS_BC}
        ${CMAKE_CURRENT_SOURCE_DIR}/snippets_div.ll
    DEPENDS tpde_encodegen ${SNIPPETS_BC} ${CMAKE_CURRENT_SOURCE_DIR}/snippets_div.ll)

add_library(qumir_codegen_tpde
    tpde_adaptor.cpp
    tpde_adaptor.h
    tpde_backend.h
    tpde_check.cpp
    tpde_check.h
    tpde_compiler_common.h
    tpde_compiler_${QUMIR_TPDE_ARCH}.cpp
    ${SNIPPETS_HPP})
target_include_directories(qumir_codegen_tpde PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
target_compile_features(qumir_codegen_tpde PUBLIC cxx_std_23)
# Not linked to qumir: qumiri and qumirc put this library ahead of their qumir link group.
target_link_libraries(qumir_codegen_tpde PRIVATE tpde)
target_compile_definitions(qumir_codegen_tpde PUBLIC QUMIR_ENABLE_TPDE)
