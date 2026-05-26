add_library(ejsc_extra STATIC
    src/timer_manager.cpp
)
add_library(ejsc::extra ALIAS ejsc_extra)

target_include_directories(ejsc_extra
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ejsc_extra PUBLIC ejsc::ejsc)

set_target_properties(ejsc_extra PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
