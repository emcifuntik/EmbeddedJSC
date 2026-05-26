add_executable(ejsc_timers main.cpp)
target_link_libraries(ejsc_timers PRIVATE ejsc::ejsc ejsc::extra)
set_target_properties(ejsc_timers PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Release"
)
