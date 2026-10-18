find_package(PkgConfig REQUIRED)

pkg_check_modules(GTEST gtest REQUIRED)

macro(ut name source)
    add_executable(${name} ${source})
    if(UNIX AND NOT APPLE)
        target_link_libraries(${name}
            "$<LINK_GROUP:RESCAN,qumir,qumir_runtime,qumir_codegen_llvm>"
            ${GTEST_LIBRARIES})
    else()
        target_link_libraries(${name}
            qumir
            qumir_runtime
            qumir_codegen_llvm
            ${GTEST_LIBRARIES})
    endif()
    target_include_directories(${name} PRIVATE ${GTEST_INCLUDE_DIRS})
    target_link_directories(${name} PRIVATE ${GTEST_LIBRARY_DIRS})
    target_compile_options(${name} PRIVATE -Wno-character-conversion)
    add_test(NAME ${name} COMMAND ${name} ${ARGN} --gtest_output=xml:${name}.xml)
endmacro()

ut(test_lexer test_lexer.cpp)
ut(test_ast_clone test_ast_clone.cpp)
ut(test_core_lexer test_core_lexer.cpp)
ut(test_json_parser test_json_parser.cpp)
ut(test_name_resolver test_name_resolver.cpp)
ut(test_cfg test_cfg.cpp)
ut(test_debug_async test_debug_async.cpp)
ut(test_vm_register_file test_vm_register_file.cpp)
ut(test_reg test_reg.cpp ${CMAKE_SOURCE_DIR}/test/regtest)
set_target_properties(test_reg PROPERTIES ENABLE_EXPORTS ON)
ut(test_field_access test_field_access.cpp)
ut(test_bit_ops test_bit_ops.cpp)
ut(test_if_expr test_if_expr.cpp)
ut(test_future test_future.cpp)
ut(test_lifetime_traits test_lifetime_traits.cpp)
ut(test_lifetime_validator test_lifetime_validator.cpp)
ut(test_lifetime_lowering test_lifetime_lowering.cpp)
ut(test_lifetime_pass test_lifetime_pass.cpp)
ut(test_printer test_printer.cpp)
ut(test_semantic_pipeline test_semantic_pipeline.cpp)
ut(test_type_annotation test_type_annotation.cpp)
ut(test_source_module_loader test_source_module_loader.cpp)
ut(test_compose test_compose.cpp)
ut(test_module_exec test_module_exec.cpp)
ut(test_ffi test_ffi.cpp)
ut(test_kernel_ast test_kernel_ast.cpp)
ut(test_byte_match test_byte_match.cpp)
ut(test_inline_attribute test_inline_attribute.cpp)
ut(test_symbol_object_cache test_symbol_object_cache.cpp)
ut(test_collect_cacheable test_collect_cacheable.cpp)
ut(test_inline_imports test_inline_imports.cpp)
ut(test_codegen_partition test_codegen_partition.cpp)
ut(test_llvm_debug_info test_llvm_debug_info.cpp)
get_target_property(QUMIR_CODEGEN_INCLUDE_DIRS qumir_codegen_llvm INCLUDE_DIRECTORIES)
target_include_directories(test_llvm_debug_info SYSTEM PRIVATE ${QUMIR_CODEGEN_INCLUDE_DIRS})
ut(test_link_and_lookup test_link_and_lookup.cpp)
ut(test_cached_compile test_cached_compile.cpp)
ut(test_cacheable_mangle test_cacheable_mangle.cpp)

# JavaScript execution tests (Node.js required)
find_program(QUMIR_NODE_EXECUTABLE node)
if(QUMIR_NODE_EXECUTABLE)
    add_test(
        NAME test_exec_js
        COMMAND ${QUMIR_NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_exec.js
            --root ${CMAKE_SOURCE_DIR}/test/regtest/
            --runtime ${CMAKE_SOURCE_DIR}/service/static/runtime/
            --wasm-dir ${CMAKE_BINARY_DIR}/wasm-output
            --xml ${CMAKE_BINARY_DIR}/test/test_js.xml
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    add_test(
        NAME test_exec_js_core_lifetime
        COMMAND ${QUMIR_NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_exec.js
            --root ${CMAKE_SOURCE_DIR}/test/regtest/
            --runtime ${CMAKE_SOURCE_DIR}/service/static/runtime/
            --wasm-dir ${CMAKE_BINARY_DIR}/wasm-output-core
            --core
            --filter corelang/lifetime*
            --xml ${CMAKE_BINARY_DIR}/test/test_js_core_lifetime.xml
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    add_test(
        NAME test_exec_js_core_module
        COMMAND ${QUMIR_NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_exec.js
            --root ${CMAKE_SOURCE_DIR}/test/regtest/
            --runtime ${CMAKE_SOURCE_DIR}/service/static/runtime/
            --wasm-dir ${CMAKE_BINARY_DIR}/wasm-output-core-module
            --core
            --filter corelang/use_module*
            --xml ${CMAKE_BINARY_DIR}/test/test_js_core_module.xml
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    # Regression for wasm32 struct layout (Ptr field offset must match the
    # real 4-byte-pointer wasm32 DataLayout, not the 8-byte default). Runs at
    # -O3 since that is where the pre-fix offset mismatch produced UB that
    # LLVM's optimizer miscompiled away.
    add_test(
        NAME test_exec_js_wasm32_struct_ptr_field
        COMMAND ${QUMIR_NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_exec.js
            --root ${CMAKE_SOURCE_DIR}/test/regtest/
            --runtime ${CMAKE_SOURCE_DIR}/service/static/runtime/
            --wasm-dir ${CMAKE_BINARY_DIR}/wasm-output-wasm32-struct
            --core
            --opt 3
            --filter corelang/wasm32_struct_ptr_field
            --xml ${CMAKE_BINARY_DIR}/test/test_js_wasm32_struct_ptr_field.xml
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()
