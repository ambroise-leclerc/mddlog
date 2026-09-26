cmake_minimum_required(VERSION 4.0.0)

# CMake still gates import std behind an experimental UUID that is specific to a CMake release
# series. Fail explicitly when this project has not reviewed a newer series instead of silently
# accepting an unknown gate value - mirrors MduX's root CMakeLists.txt (the reference support
# matrix this project targets, see issue #5).
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.4")
    message(FATAL_ERROR
        "CMake ${CMAKE_VERSION} has not been qualified for mddlog's import std support; use a 4.0-4.3 release.")
endif()
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
else()
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/PreventInSourceBuilds.cmake)

project(mddlog VERSION 0.1.0 DESCRIPTION "Modern C++23 Modules Logger Library for Medical Devices" HOMEPAGE_URL "https://github.com/ambroise-leclerc/mddlog" LANGUAGES CXX)

message(STATUS "CMAKE_EXPERIMENTAL_CXX_IMPORT_STD: ${CMAKE_EXPERIMENTAL_CXX_IMPORT_STD}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# Compiler version requirements for C++23 modules support. See the reference support matrix in
# issue #5: Windows/MSVC, Linux/GCC, Linux/Clang and macOS/Clang are the tested configurations;
# these checks are admission floors, not a claim that every version above them is verified.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "19.40")
        message(FATAL_ERROR "MSVC 17.14 (Visual Studio 2022 version 17.10) or later is required for C++23 modules support. Current version: ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC 16.1, not 15 and not a bare 16.0: GCC 15 cannot round-trip libstdc++'s std module through
    # a second-level BMI, which is exactly what building the SpecLab-based test suite requires
    # (SpecLab's own floor is GCC 16.1 for the same reason). GCC 16.2 corrupted the BMIs this
    # project's modules produce ("failed to read compiled module cluster ... Bad file data"), which
    # is why CI pins the exact gcc:16.1.0 container rather than a floating gcc:16 tag - see
    # .github/workflows/linux-gcc16-build.yml.
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.1")
        message(FATAL_ERROR "GCC 16.1 or later is required for C++23 modules support. Current version: ${CMAKE_CXX_COMPILER_VERSION}")
    elseif(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "16.2" AND
           CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.3")
        message(FATAL_ERROR
            "GCC 16.2 is unsupported: it corrupts this project's module BMIs ('failed to read "
            "compiled module cluster ... Bad file data'). Use GCC 16.1 (CI pins gcc:16.1.0 "
            "exactly for the same reason). Current version: ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "20.0")
        message(FATAL_ERROR "Clang 20 or later is required for C++23 modules support. Current version: ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    message(FATAL_ERROR
        "AppleClang is unsupported. Apple Silicon macOS requires upstream LLVM/Clang 21.1.8; "
        "configure with --preset ninja-macos-clang.")
else()
    message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. mddlog requires MSVC 17.14+, GCC 16.1+, or upstream Clang 20+ for C++23 modules support.")
endif()

if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "Unsupported Apple target '${CMAKE_SYSTEM_NAME}'. mddlog supports Apple Silicon macOS only.")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        message(FATAL_ERROR "mddlog supports macOS only on Apple Silicon (arm64).")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "mddlog on macOS requires upstream LLVM/Clang 21.1.8; AppleClang and GCC are unsupported.")
    endif()
    if(NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "21.1.8")
        message(FATAL_ERROR
            "mddlog's verified macOS compiler is upstream Clang 21.1.8 exactly; found "
            "${CMAKE_CXX_COMPILER_VERSION}.")
    endif()
    if(NOT CMAKE_VERSION VERSION_EQUAL "4.3.1")
        message(FATAL_ERROR "mddlog's verified macOS CMake is 4.3.1 exactly; found ${CMAKE_VERSION}.")
    endif()
    function(_mddlog_verify_llvm_archive_tool tool_path expected_name)
        get_filename_component(tool_name "${tool_path}" NAME)
        if(NOT tool_name STREQUAL expected_name)
            message(FATAL_ERROR
                "The macOS build requires ${expected_name}, but CMake selected '${tool_path}'. "
                "Configure with --preset ninja-macos-clang.")
        endif()

        execute_process(
            COMMAND "${tool_path}" --version
            RESULT_VARIABLE tool_result
            OUTPUT_VARIABLE tool_stdout
            ERROR_VARIABLE tool_stderr
        )
        string(CONCAT tool_banner "${tool_stdout}" "\n" "${tool_stderr}")
        if(NOT tool_result EQUAL 0 OR
           NOT tool_banner MATCHES "LLVM version 21\\.1\\.8([^0-9.]|$)")
            message(FATAL_ERROR
                "The macOS build requires LLVM 21.1.8 ${expected_name}; '${tool_path} --version' "
                "returned '${tool_banner}'.")
        endif()
    endfunction()

    _mddlog_verify_llvm_archive_tool("${CMAKE_AR}" "llvm-ar")
    _mddlog_verify_llvm_archive_tool("${CMAKE_RANLIB}" "llvm-ranlib")
endif()

# Checked after the compiler/platform diagnostics so unsupported combinations get the specific
# remediation above rather than a generic missing-import-metadata error.
if(23 IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
    message(STATUS "C++23 import std metadata available")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "19.40")
    message(STATUS "__CMAKE::CXX23 target NOT available, but MSVC ${CMAKE_CXX_COMPILER_VERSION} should support import std with /experimental:module")
else()
    message(FATAL_ERROR
        "The selected compiler and standard library do not provide C++23 import std support. "
        "CMAKE_CXX_COMPILER_IMPORT_STD='${CMAKE_CXX_COMPILER_IMPORT_STD}'.")
endif()

# Generator validation.
#
# CMake implements C++ modules for the Ninja, Ninja Multi-Config and Visual Studio 17.4+
# generators only, and Visual Studio is ruled out separately because it cannot do `import std`.
# That leaves the Ninja family. Without this check, a plain `cmake ..` on Linux picks "Unix
# Makefiles" and fails later inside a try_compile with a CMake-internal backtrace that never
# mentions the generator.
if(NOT CMAKE_GENERATOR MATCHES "Ninja")
    message(FATAL_ERROR
        "mddlog requires the Ninja generator, but CMake selected \"${CMAKE_GENERATOR}\".\n"
        "C++23 modules and `import std` are not supported by that generator.\n"
        "\n"
        "Configure with:\n"
        "    cmake .. -G Ninja\n"
        "\n"
        "Or set it once for the shell and omit the flag from then on:\n"
        "    export CMAKE_GENERATOR=Ninja        # Windows: set CMAKE_GENERATOR=Ninja\n"
        "\n"
        "If Ninja is missing: `apt install ninja-build`, `dnf install ninja-build`,\n"
        "or `winget install Ninja-build.Ninja`.")
endif()

# Platform validation
if(NOT WIN32 AND
   NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux") AND
   NOT (CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
    message(FATAL_ERROR "mddlog only supports Windows, Linux, and Apple Silicon macOS.")
endif()

# C++23 Modules support
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
# Initialize every subsequently-created C++ target with import std support. Checking for CMake's
# internal target here is too early on Clang: CMake creates it while applying this property.
if(23 IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
    set(CMAKE_CXX_MODULE_STD ON)
endif()

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
include(CPM)
include(CompilerSettings)
include(Linker)
include(CompilerWarnings)
include(Sanitizers)
include(StaticAnalyzers)
include(Doxygen)
include(DependencySummaryTable)

# Create module library targets: mddlog-core (governed, no sink/adapter module) and mddlog
# (adapter, links mddlog-core PUBLIC) - the boundary ADR-001 Decision 6 requires so a consumer
# (MduX being the motivating one) can import and link the governed core without pulling in
# SimpleLogger, the sinks, or their dependencies. mddlog-core -> mddlog is a one-way dependency:
# nothing under mddlog.core.* may import a mddlog.sinks.* or mddlog.adapter.* module (enforced
# below and by the build.core.* tests in tests/CMakeLists.txt).
add_library(mddlog-core)
add_library(mddlog::core ALIAS mddlog-core)
# EXPORT_NAME, not the target name, controls the name after the mddlog:: namespace on install
# (see install(EXPORT ...) below) - this keeps the installed imported target the same
# mddlog::core spelling as the in-tree ALIAS above, despite the real target being named
# "mddlog-core" (a bare "core" target name would be too generic/collision-prone in-tree).
set_target_properties(mddlog-core PROPERTIES EXPORT_NAME core)

add_library(mddlog)
add_library(mddlog::mddlog ALIAS mddlog)

add_library(mddlog_options INTERFACE)
add_library(mddlog_warnings INTERFACE)

# mddlog-core: governed modules only, in dependency order. Nothing here may import a sink or
# adapter module.
set(MDDLOG_CORE_MODULE_FILES
    include/mddlog/core/InlineString.cppm
    include/mddlog/core/LogLevel.cppm
    include/mddlog/core/WriteResult.cppm
    include/mddlog/core/Record.cppm
    include/mddlog/core/Ring.cppm
)
target_sources(mddlog-core
    PUBLIC
    FILE_SET cxx_modules TYPE CXX_MODULES FILES
        ${MDDLOG_CORE_MODULE_FILES}
)

# mddlog: adapter/sink modules, in dependency order.
target_sources(mddlog
    PUBLIC
    FILE_SET cxx_modules TYPE CXX_MODULES FILES
        include/mddlog/adapter/LogRecord.cppm
        include/mddlog/sinks/Sink.cppm
        include/mddlog/adapter/RingDrain.cppm
        include/mddlog/sinks/ConsoleSink.cppm
        include/mddlog/adapter/Logger.cppm
        include/mddlog/mddlog.cppm
        include/mddlog/Log.cppm
)
# Configuration application
configure_compiler_settings(mddlog_options)
configure_medical_compliance(mddlog_options ${PROJECT_VERSION_MAJOR} ${PROJECT_VERSION_MINOR} ${PROJECT_VERSION_PATCH})
configure_linker(mddlog_options)
set_project_warnings(mddlog_warnings)

enable_sanitizers(mddlog_options)

# Set target properties
target_include_directories(mddlog-core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(mddlog PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(mddlog-core PUBLIC cxx_std_23)
target_compile_features(mddlog PUBLIC cxx_std_23)

if(23 IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
    set_target_properties(mddlog-core mddlog PROPERTIES CXX_MODULE_STD ON)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(mddlog-core PRIVATE /experimental:module /std:c++latest)
    target_compile_options(mddlog PRIVATE /experimental:module /std:c++latest)
endif()

find_package(Threads REQUIRED)
target_link_libraries(mddlog_options INTERFACE Threads::Threads)

# Apply compiler settings. mddlog_options is PUBLIC because it carries Threads::Threads, which a
# consumer's own final link needs too; mddlog_warnings (this project's -Wall/-Werror/etc.) is
# PRIVATE so those flags apply while building mddlog itself and never leak into a consumer's
# compile of its own code - a downstream project should choose its own warning policy.
target_link_libraries(mddlog-core PUBLIC mddlog_options)
target_link_libraries(mddlog-core PRIVATE mddlog_warnings)

target_link_libraries(mddlog PUBLIC mddlog_options mddlog::core)
target_link_libraries(mddlog PRIVATE mddlog_warnings)

# Early direct-link guard: mddlog-core must never link mddlog. The build.core.graph CTest adds
# evaluated target dependencies and compiler-produced imports after the build. Here a regression
# fails the build immediately rather than waiting for `ctest` to run. Checks both LINK_LIBRARIES
# (a PUBLIC/PRIVATE target_link_libraries entry) and INTERFACE_LINK_LIBRARIES (an INTERFACE one,
# which LINK_LIBRARIES alone would miss even though it still makes mddlog-core depend on mddlog
# for consumers), and both spellings a stray target_link_libraries call could use.
set(_mddlog_forbidden_link_names mddlog mddlog::mddlog)
foreach(_mddlog_core_link_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_mddlog_core_link_libraries mddlog-core ${_mddlog_core_link_property})
    if(_mddlog_core_link_libraries)
        foreach(_mddlog_forbidden_name IN LISTS _mddlog_forbidden_link_names)
            if(_mddlog_forbidden_name IN_LIST _mddlog_core_link_libraries)
                message(FATAL_ERROR
                    "mddlog-core must not link mddlog (found '${_mddlog_forbidden_name}' in "
                    "${_mddlog_core_link_property}): this would make the governed/adapter split "
                    "(ADR-001 Decision 6) a one-way dependency in name only.")
            endif()
        endforeach()
    endif()
endforeach()
unset(_mddlog_core_link_libraries)
unset(_mddlog_core_link_property)
unset(_mddlog_forbidden_link_names)
unset(_mddlog_forbidden_name)

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

    # Report the full and core-only installed consumers separately. Their scratch prefixes differ,
    # but every `cmake --install` of this build tree rewrites ${CMAKE_BINARY_DIR}/install_manifest.txt,
    # so a shared resource lock keeps the two installs from writing that file concurrently.
    foreach(_mddlog_consumer_kind IN ITEMS full core)
        if(_mddlog_consumer_kind STREQUAL "full")
            set(_mddlog_consumer_test InstallTreeConsumer)
        else()
            set(_mddlog_consumer_test InstallTreeCoreConsumer)
        endif()
        # NAME/COMMAND form so $<CONFIG> names the configuration CTest is testing, which a
        # multi-config generator (Ninja Multi-Config) needs for install, build and run paths.
        add_test(NAME ${_mddlog_consumer_test}
            COMMAND "${CMAKE_COMMAND}"
            -D "BUILD_DIR=${CMAKE_BINARY_DIR}"
            -D "CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            -D "AR=${CMAKE_AR}"
            -D "RANLIB=${CMAKE_RANLIB}"
            -D "STDLIB_MODULES_JSON=${CMAKE_CXX_STDLIB_MODULES_JSON}"
            -D "OSX_SYSROOT=${CMAKE_OSX_SYSROOT}"
            # The consumer must select the same standard library as this build, not the compiler's
            # default: on Linux libc++ is not Clang's default, so a consumer built without this flag
            # scans libc++'s std.cppm with libstdc++'s include path and fails on a missing '__config'.
            -D "CXX_FLAGS=${CMAKE_CXX_FLAGS}"
            # MSVC's runtime-library selection (/MD vs /MT, Release vs Debug) is driven by the
            # configuration. Without forwarding it, the consumer configures with an empty build
            # type while this build used Release, the two pick different default runtime libraries,
            # and linking the consumer against the installed mddlog.lib fails with LNK4098/LNK1319.
            -D "CONFIG=$<CONFIG>"
            -D "GENERATOR=${CMAKE_GENERATOR}"
            -D "EXPECTED_VERSION=${PROJECT_VERSION}"
            -D "SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            -D "CONSUMER_KIND=${_mddlog_consumer_kind}"
            -P "${CMAKE_SOURCE_DIR}/cmake/TestInstallConsumer.cmake"
        )
        set_tests_properties(${_mddlog_consumer_test} PROPERTIES LABELS "build" RESOURCE_LOCK mddlog_install_tree)
    endforeach()
endif()

# Installation
include(GNUInstallDirs)

install(TARGETS mddlog mddlog-core mddlog_options mddlog_warnings
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
