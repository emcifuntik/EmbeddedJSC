add_executable(ejsc_native_module main.cpp)
target_link_libraries(ejsc_native_module PRIVATE ejsc::ejsc)
set_target_properties(ejsc_native_module PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Release"
)
add_custom_command(TARGET ejsc_native_module POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/test.js"
        "$<TARGET_FILE_DIR:ejsc_native_module>/test.js"
    VERBATIM)
