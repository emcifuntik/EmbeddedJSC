add_executable(ejsc_test_eval test_eval.cpp)
target_link_libraries(ejsc_test_eval PRIVATE ejsc::ejsc)
set_target_properties(ejsc_test_eval PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Release"
)
add_test(NAME ejsc_test_eval COMMAND ejsc_test_eval)
