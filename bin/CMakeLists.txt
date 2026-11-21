find_package(PkgConfig REQUIRED)
pkg_check_modules(READLINE IMPORTED_TARGET QUIET readline)

add_executable(qumiri interpreter.cpp)
add_executable(qumirc driver.cpp)
if(QUMIR_ENABLE_TPDE)
    # Ahead of the link group below, which resolves its qumir symbols.
    target_link_libraries(qumiri PUBLIC qumir_codegen_tpde)
    target_link_libraries(qumirc PUBLIC qumir_codegen_tpde)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(qumiri PUBLIC
        "$<LINK_GROUP:RESCAN,qumir,qumir_runtime,qumir_codegen_llvm>")
    target_link_libraries(qumirc PUBLIC
        "$<LINK_GROUP:RESCAN,qumir,qumir_runtime,qumir_codegen_llvm>")
else()
    target_link_libraries(qumiri PUBLIC qumir qumir_runtime qumir_codegen_llvm)
    target_link_libraries(qumirc PUBLIC qumir qumir_runtime qumir_codegen_llvm)
endif()
target_link_libraries(qumiri PRIVATE PkgConfig::READLINE)

target_compile_definitions(qumirc PRIVATE QUMIR_VERSION_STRING="${QUMIR_VERSION_STRING}")
target_compile_definitions(qumiri PRIVATE QUMIR_VERSION_STRING="${QUMIR_VERSION_STRING}")

install(TARGETS qumiri qumirc
    RUNTIME DESTINATION ${QUMIR_PRIVATE_BINDIR}
    COMPONENT compiler)

# The real binaries stay in the versioned tree; /proc/self/exe resolves these links,
# so qumirc still finds libqumir_runtime.a relative to itself.
install(CODE "
    set(_bindir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin\")
    file(MAKE_DIRECTORY \"\${_bindir}\")
    file(CREATE_LINK \"../${QUMIR_PRIVATE_BINDIR}/qumirc\" \"\${_bindir}/qumirc\" SYMBOLIC)
    file(CREATE_LINK \"../${QUMIR_PRIVATE_BINDIR}/qumiri\" \"\${_bindir}/qumiri\" SYMBOLIC)
" COMPONENT compiler)
