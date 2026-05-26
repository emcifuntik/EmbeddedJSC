cmake_minimum_required(VERSION 3.21)

project(embeddedjsc
    VERSION 0.2.0
    LANGUAGES CXX C
    DESCRIPTION "Embeddable JavaScriptCore with a QuickJS-class C++ API"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CONFIGURATION_TYPES Debug Release CACHE STRING "" FORCE)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/LIB)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/LIB)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/BIN)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(EJSC_BUILD_EXAMPLES "Build EmbeddedJSC examples" ON)
option(EJSC_BUILD_TESTS    "Build EmbeddedJSC tests"    ON)
option(EJSC_INSTALL        "Generate install rules"     ON)

find_package(JavaScriptCore CONFIG REQUIRED)
find_file(JSC_CMAKE_CONFIG_FILE NAMES cmakeconfig.h REQUIRED)

add_library(ejsc STATIC
    src/runtime.cpp
    src/context.cpp
    src/value.cpp
    src/module.cpp
    src/internal/jsc_init.cpp
    src/internal/global_object_table.cpp
    src/internal/module_loader.cpp
    src/internal/synthetic_module.cpp
    src/internal/class_bridge.cpp
)
add_library(ejsc::ejsc ALIAS ejsc)

target_include_directories(ejsc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(ejsc PUBLIC
    JavaScriptCore::JavaScriptCore
    JavaScriptCore::WTF
    JavaScriptCore::bmalloc
    JavaScriptCore::icu-dt
    JavaScriptCore::icu-in
    JavaScriptCore::icu-uc
    ws2_32
    winmm
    dbghelp
    Synchronization
    clang_rt.builtins-x86_64.lib
)

target_compile_definitions(ejsc PUBLIC
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _MT
    $<$<CONFIG:Debug>:_DEBUG>
    STATICALLY_LINKED_WITH_JavaScriptCore
    STATICALLY_LINKED_WITH_WTF
    STATICALLY_LINKED_WITH_bmalloc
)

# The /FI cmakeconfig.h force-include is needed for any TU pulling in JSC
# headers. For our own build we use the path that find_file resolved above.
# For installed consumers, ejscConfig.cmake re-runs find_file and appends
# the right option to the imported target's INTERFACE_COMPILE_OPTIONS, so
# we only emit this for the BUILD_INTERFACE here.
target_compile_options(ejsc PUBLIC
    $<BUILD_INTERFACE:/FI${JSC_CMAKE_CONFIG_FILE}>
)

set_target_properties(ejsc PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_subdirectory(extra)

if(EJSC_BUILD_EXAMPLES)
    add_subdirectory(examples/hello)
    add_subdirectory(examples/native_module)
    add_subdirectory(examples/timers)
    add_subdirectory(examples/classes)
endif()

if(EJSC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ----------------------------------------------------------------------------
# Install + export
# ----------------------------------------------------------------------------

if(EJSC_INSTALL)
    install(TARGETS ejsc ejsc_extra
        EXPORT ejscTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    install(DIRECTORY include/ejsc
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY extra/include/ejsc
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT ejscTargets
        FILE   ejscTargets.cmake
        NAMESPACE ejsc::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ejsc
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ejscConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/ejscConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ejsc
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/ejscConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ejscConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ejscConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ejsc
    )
endif()
