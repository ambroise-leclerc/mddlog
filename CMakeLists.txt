cmake_minimum_required(VERSION 4.0.0)

# CRITICAL: Must be the first thing after cmake_minimum_required for toolchain detection
# Official UUID for CMake 4.0.3 import std support
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/PreventInSourceBuilds.cmake)

project(mddlog VERSION 0.1.0 DESCRIPTION "Modern C++23 Modules Logger Library for Medical Devices" HOMEPAGE_URL "https://github.com/ambroise-leclerc/mddlog" LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

# Debug: Check if import std support was detected
message(STATUS "CMAKE_EXPERIMENTAL_CXX_IMPORT_STD: ${CMAKE_EXPERIMENTAL_CXX_IMPORT_STD}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
if(TARGET __CMAKE::CXX23)
    message(STATUS "__CMAKE::CXX23 target available")
else()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "19.40")
        message(STATUS "__CMAKE::CXX23 target NOT available, but MSVC ${CMAKE_CXX_COMPILER_VERSION} should support import std with /experimental:module")
    else()
        message(WARNING "__CMAKE::CXX23 target NOT available - import std; will not work")
    endif()
endif()

# Compiler version requirements for C++23 modules support
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "19.40")
        message(FATAL_ERROR "MSVC 17.14 (Visual Studio 2022 version 17.10) or later is required for C++23 modules support. Current version: ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "15.0")
        message(FATAL_ERROR "GCC 15 or later is required for C++23 modules support. Current version: ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "20.0")
        message(FATAL_ERROR "Clang 20 or later is required for C++23 modules support. Current version: ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
else()
    message(WARNING "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. MduX requires MSVC 17.14+, GCC 15+, or Clang 20+ for C++23 modules support.")
endif()


# C++23 Modules support
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

# Compiler-specific modules configuration
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fmodules>)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # MSVC-specific modules configuration for C++23 and import std
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/experimental:module>)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/std:c++latest>)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(StandardProjectSettings)
include(Cache)
include(CompilerSettings)
include(Linker)
include(CompilerWarnings)
include(Sanitizers)
include(StaticAnalyzers)



# Create module library target
add_library(mddlog)
add_library(mddlog::mddlog ALIAS mddlog)
add_library(mddlog_options INTERFACE)
add_library(mddlog_warnings INTERFACE)

# Add module source files in dependency order
target_sources(mddlog
    PUBLIC
    FILE_SET cxx_modules TYPE CXX_MODULES FILES
        include/mddlog/core/LogLevel.cppm
        include/mddlog/core/LogRecord.cppm
        include/mddlog/sinks/Sink.cppm
        include/mddlog/sinks/ConsoleSink.cppm
        include/mddlog/core/Logger.cppm
        include/mddlog/mddlog.cppm
)
# Configuration application
configure_compiler_settings(mddlog_options)
configure_medical_compliance(mddlog_options ${PROJECT_VERSION_MAJOR} ${PROJECT_VERSION_MINOR} ${PROJECT_VERSION_PATCH})
configure_linker(mddlog_options)
set_project_warnings(mddlog_warnings)

enable_sanitizers(mddlog_options)

# Set target properties
target_include_directories(mddlog PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(mddlog PUBLIC cxx_std_23)

# Build the std module first for GCC
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Create a custom target to build the std module
    add_custom_target(build_std_module
        COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fmodules-ts -x c++-system-header iostream
        COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fmodules-ts -x c++-system-header vector
        COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fmodules-ts -x c++-system-header string
        COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fmodules-ts -x c++-system-header memory
        COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fmodules-ts -x c++-system-header thread
        COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fmodules-ts -x c++-system-header mutex
        COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fmodules-ts -x c++-system-header atomic
        COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fmodules-ts -x c++-system-header chrono
        COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fmodules-ts -x c++-system-header source_location
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Building std module for GCC"
    )
endif()

# Link to the std module if available
if(TARGET __CMAKE::CXX23)
    target_link_libraries(mddlog PUBLIC __CMAKE::CXX23)
endif()

# Apply compiler settings
target_link_libraries(mddlog PUBLIC mddlog_options mddlog_warnings)

# Build options
option(MDDLOG_BUILD_EXAMPLES "Build MddLog examples" ON)
option(MDDLOG_BUILD_TESTS "Build MddLog tests" ON)

# Add subdirectories
if(MDDLOG_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(MDDLOG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Installation
include(GNUInstallDirs)

install(TARGETS mddlog mddlog_options mddlog_warnings
    EXPORT mddlogTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILE_SET cxx_modules DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/modules
)

install(EXPORT mddlogTargets
    FILE mddlogTargets.cmake
    NAMESPACE mddlog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mddlog
)

# Create config file
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/mddlogConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mddlogConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/mddlogConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mddlog
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/mddlogConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/mddlogConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mddlog
)

