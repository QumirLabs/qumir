find_package(LLVM REQUIRED CONFIG)

add_executable(server server.cpp)
# Link to our libs; LLVM libs come transitively via qumir_codegen_llvm
if(UNIX AND NOT APPLE)
    target_link_libraries(server PUBLIC
        "$<LINK_GROUP:RESCAN,qumir,qumir_runtime,qumir_codegen_llvm>"
        coroio)
else()
    target_link_libraries(server PUBLIC qumir qumir_runtime qumir_codegen_llvm coroio)
endif()

# Headers and defs for using llvm/Support headers directly in server
target_include_directories(server PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(server PRIVATE ${LLVM_DEFINITIONS}
    QUMIR_VERSION_STRING="${QUMIR_VERSION_STRING}")

# Plugins resolve coroio and qumir symbols from the executable itself.
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
