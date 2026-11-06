set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "qumir")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Alexey Ozeritskiy <aozeritsky@gmail.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://qumir.dev")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Educational programming language with Russian keywords")
set(CPACK_STRIP_FILES ON)

set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# Yields 1.1.0-42; the hyphen is the Debian revision separator.
set(CPACK_DEBIAN_PACKAGE_RELEASE "${QUMIR_BUILD_NUMBER}")

set(CPACK_COMPONENTS_ALL compiler)

set(CPACK_DEBIAN_COMPILER_PACKAGE_NAME "qumir")
set(CPACK_DEBIAN_COMPILER_PACKAGE_SECTION "devel")
# qumirc links the produced object file by invoking c++, and needs wasm-ld for --target=wasm.
set(CPACK_DEBIAN_COMPILER_PACKAGE_DEPENDS "g++")
set(CPACK_DEBIAN_COMPILER_PACKAGE_RECOMMENDS "lld")
# The old qumir-service ships its own copy of the JS runtime, which would not match this compiler.
set(CPACK_DEBIAN_COMPILER_PACKAGE_BREAKS "qumir-service (<< 1.1.0)")
set(CPACK_COMPONENT_COMPILER_DESCRIPTION
    "Qumir compiler and interpreter.\n Compiles the KUMIR-style educational language to native code,\n WebAssembly or runs it through the IR interpreter and the LLVM JIT.")

include(CPack)
