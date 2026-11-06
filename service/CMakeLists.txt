find_package(LLVM REQUIRED CONFIG)

add_executable(server server.cpp)
# The compiler runs as a qumirc subprocess; the server needs only coroio and LLVMSupport (llvm::json).
if(QUMIR_LLVM_LINK_SHARED)
    if(NOT TARGET LLVM)
        message(FATAL_ERROR
            "QUMIR_LLVM_LINK_SHARED=ON, but this LLVM installation ships no shared "
            "libLLVM (check llvm-config --shared-mode)")
    endif()
    target_link_libraries(server PUBLIC coroio LLVM)
else()
    llvm_map_components_to_libnames(SERVER_LLVM_LIBS support)
    target_link_libraries(server PUBLIC coroio ${SERVER_LLVM_LIBS})
endif()

# Headers and defs for using llvm/Support headers directly in server
target_include_directories(server PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(server PRIVATE ${LLVM_DEFINITIONS}
    QUMIR_VERSION_STRING="${QUMIR_VERSION_STRING}")

# Plugins resolve coroio and llvm::json symbols from the executable itself.
set_target_properties(server PROPERTIES ENABLE_EXPORTS ON)
target_link_libraries(server PUBLIC ${CMAKE_DL_LIBS})

install(PROGRAMS $<TARGET_FILE:server>
    DESTINATION bin
    RENAME qumir-service
    COMPONENT service)

# static/docs and static/runtime are symlinks into the source tree, so ship the real files in their place.
install(DIRECTORY static/
    DESTINATION share/qumir-service/static
    COMPONENT service
    REGEX "/docs$" EXCLUDE
    REGEX "/runtime$" EXCLUDE)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../qumir/runtime/js/
    DESTINATION share/qumir-service/static/runtime
    COMPONENT service)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../docs/ru/
    DESTINATION share/qumir-service/static/docs
    COMPONENT service)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../docs/arch/
    DESTINATION share/qumir-service/static/docs/arch
    COMPONENT service)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../examples/
    DESTINATION share/qumir-service/examples
    COMPONENT service)

configure_file(qumir-service.service.in qumir-service.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/qumir-service.service
    DESTINATION lib/systemd/system
    COMPONENT service)
