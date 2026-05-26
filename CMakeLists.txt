cmake_minimum_required(VERSION 3.21)

project(embeddedjsc
    VERSION 0.1.0
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

option(EJSC_BUILD_EXAMPLES "Build EmbeddedJSC examples" ON)
option(EJSC_BUILD_TESTS    "Build EmbeddedJSC tests"    ON)

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
)
add_library(ejsc::ejsc ALIAS ejsc)

target_include_directories(ejsc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
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

target_compile_options(ejsc PUBLIC
    /FI${JSC_CMAKE_CONFIG_FILE}
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
endif()

if(EJSC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
