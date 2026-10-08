# TestInstallConsumer.cmake
#
# Proves that mddlog is consumable as a package, not just that `cmake --install` exits 0: it
# configures, builds and RUNS the independent consumer project tests/consumer/package against
#   full     the installed package, components core+full (+file_storage/audit_tool when built);
#   core     the installed package, component core only, linking mddlog::core alone;
#   source   this source tree, added by the consumer with add_subdirectory();
#   archive  a source archive made by scripts/package-source.py from the tracked files, extracted
#            and added with add_subdirectory(), so the shipped file list is itself exercised.
# The consumer applies its own strict warnings to its own sources and, away from MSVC, another
# configuration than the library's, while mddlog's warnings must not reach it (#121, ADR-007).
#
# Run via: cmake -D BUILD_DIR=... -D CXX_COMPILER=... -D GENERATOR=...
#              -D EXPECTED_VERSION=... -D SOURCE_DIR=... -D CONSUMER_KIND=full|core|source|archive
#              [-D CONFIG=<configuration under test>] [-D FILE_STORAGE=ON|OFF] [-D AUDIT_TOOL=ON|OFF]
#              [-D PYTHON=<interpreter, for archive>]
#              -P TestInstallConsumer.cmake
# (see the consumer add_test() calls in the top-level CMakeLists.txt)

foreach(required_var BUILD_DIR CXX_COMPILER GENERATOR EXPECTED_VERSION SOURCE_DIR CONSUMER_KIND)
    if(NOT DEFINED ${required_var})
        message(FATAL_ERROR "TestInstallConsumer.cmake: ${required_var} must be set with -D")
    endif()
endforeach()
if(NOT CONSUMER_KIND MATCHES "^(full|core|source|archive)$")
    message(FATAL_ERROR "CONSUMER_KIND must be full, core, source or archive")
endif()
if(CONSUMER_KIND STREQUAL "archive" AND (NOT DEFINED PYTHON OR PYTHON STREQUAL ""))
    message(FATAL_ERROR "CONSUMER_KIND=archive needs -D PYTHON=<interpreter>")
endif()
foreach(optional_flag FILE_STORAGE AUDIT_TOOL)
    if(NOT DEFINED ${optional_flag})
        set(${optional_flag} OFF)
    endif()
endforeach()

set(work_dir "${BUILD_DIR}/_test_consumer_${CONSUMER_KIND}")
set(install_prefix "${work_dir}/prefix")
set(consumer_build "${work_dir}/build")
set(test_names_full InstallTreeConsumer)
set(test_names_core InstallTreeCoreConsumer)
set(test_names_source SourceSubdirectoryConsumer)
set(test_names_archive SourceArchiveConsumer)
set(test_name "${test_names_${CONSUMER_KIND}}")

# Empty for a single-config build without CMAKE_BUILD_TYPE. With a multi-config generator it is
# the configuration CTest runs (-C), used to install, build and locate that configuration only.
set(config "")
if(DEFINED CONFIG)
    set(config "${CONFIG}")
endif()
set(config_arguments)
if(NOT config STREQUAL "")
    set(config_arguments --config "${config}")
endif()

# The consumer chooses its own configuration. On GCC/Clang an installed Release library serves a
# Debug consumer (and the reverse): BMIs are rebuilt by the consumer and no runtime library is
# selected by the configuration. MSVC selects its runtime library (/MD vs /MDd) from the
# configuration, so there the consumer keeps the library's (LNK2038/LNK4098 otherwise).
set(consumer_config "${config}")
if(NOT CXX_COMPILER MATCHES "(^|[/\\\\])cl(\\.exe)?$" AND NOT CONSUMER_KIND MATCHES "^(source|archive)$")
    if(config STREQUAL "Debug")
        set(consumer_config Release)
    else()
        set(consumer_config Debug)
    endif()
endif()
set(consumer_config_arguments)
if(NOT consumer_config STREQUAL "")
    set(consumer_config_arguments --config "${consumer_config}")
endif()

file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}")

function(run_step description)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${test_name}: ${description} failed (${result}):\n${output}\n${error}")
    endif()
endfunction()

set(mode_arguments)
if(CONSUMER_KIND MATCHES "^(full|core)$")
    message(STATUS "${test_name}: installing to ${install_prefix}")
    run_step("cmake --install" "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${install_prefix}" ${config_arguments})
    set(build_info "${install_prefix}/lib/cmake/mddlog/mddlog-build-info.json")
    if(NOT EXISTS "${build_info}")
        file(GLOB_RECURSE build_info "${install_prefix}/*/mddlog-build-info.json")
    endif()
    if(NOT build_info)
        message(FATAL_ERROR "${test_name}: the installed package has no mddlog-build-info.json")
    endif()
    file(READ "${build_info}" build_info_content)
    string(JSON installed_version GET "${build_info_content}" version)
    if(NOT installed_version STREQUAL EXPECTED_VERSION)
        message(FATAL_ERROR "${test_name}: build info reports ${installed_version}, expected ${EXPECTED_VERSION}")
    endif()
    message(STATUS "${test_name}: build info ${build_info_content}")
    set(profile ${CONSUMER_KIND})
    list(APPEND mode_arguments -DMDDLOG_CONSUMER_MODE=installed "-DCMAKE_PREFIX_PATH=${install_prefix}")
else()
    set(profile full)
    set(source_tree "${SOURCE_DIR}")
    if(CONSUMER_KIND STREQUAL "archive")
        message(STATUS "${test_name}: packaging the tracked source files")
        run_step("scripts/package-source.py" "${PYTHON}" -B "${SOURCE_DIR}/scripts/package-source.py"
            --worktree --output "${work_dir}/dist")
        file(GLOB archives "${work_dir}/dist/mddlog-*-src.tar.gz")
        list(LENGTH archives archive_count)
        if(NOT archive_count EQUAL 1)
            message(FATAL_ERROR "${test_name}: expected one source archive, found '${archives}'")
        endif()
        file(MAKE_DIRECTORY "${work_dir}/extracted")
        run_step("archive extraction" "${CMAKE_COMMAND}" -E chdir "${work_dir}/extracted"
            "${CMAKE_COMMAND}" -E tar xzf "${archives}")
        file(GLOB source_tree LIST_DIRECTORIES true "${work_dir}/extracted/mddlog-*")
        if(NOT EXISTS "${source_tree}/CMakeLists.txt" OR NOT EXISTS "${source_tree}/SOURCE_REVISION")
            message(FATAL_ERROR "${test_name}: the archive lacks CMakeLists.txt or SOURCE_REVISION")
        endif()
    endif()
    list(APPEND mode_arguments -DMDDLOG_CONSUMER_MODE=source "-DMDDLOG_SOURCE_DIR=${source_tree}")
endif()

set(consumer_toolchain_arguments)
foreach(forwarded IN ITEMS AR RANLIB STDLIB_MODULES_JSON OSX_SYSROOT CXX_FLAGS)
    set(cmake_names_AR CMAKE_AR)
    set(cmake_names_RANLIB CMAKE_RANLIB)
    set(cmake_names_STDLIB_MODULES_JSON CMAKE_CXX_STDLIB_MODULES_JSON)
    set(cmake_names_OSX_SYSROOT CMAKE_OSX_SYSROOT)
    # Carries -stdlib=libc++ where the toolchain sets it: the consumer selects the standard library
    # mddlog was built with, it is not inherited from the package.
    set(cmake_names_CXX_FLAGS CMAKE_CXX_FLAGS)
    if(DEFINED ${forwarded} AND NOT "${${forwarded}}" STREQUAL "")
        list(APPEND consumer_toolchain_arguments "-D${cmake_names_${forwarded}}=${${forwarded}}")
    endif()
endforeach()
if(NOT consumer_config STREQUAL "")
    # A multi-config consumer ignores CMAKE_BUILD_TYPE and receives the configuration through --config.
    list(APPEND consumer_toolchain_arguments "-DCMAKE_BUILD_TYPE=${consumer_config}")
endif()

set(consumer_profile_arguments "-DMDDLOG_CONSUMER_PROFILE=${profile}"
    "-DMDDLOG_CONSUMER_EXPECTED_VERSION=${EXPECTED_VERSION}")
if(profile STREQUAL "full")
    list(APPEND consumer_profile_arguments "-DMDDLOG_CONSUMER_FILE_STORAGE=${FILE_STORAGE}"
        "-DMDDLOG_CONSUMER_AUDIT_TOOL=${AUDIT_TOOL}")
endif()

message(STATUS "${test_name}: configuring the consumer (${profile}, configuration '${consumer_config}')")
run_step("consumer configure" "${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/consumer/package"
    -B "${consumer_build}" -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
    ${consumer_toolchain_arguments} ${mode_arguments} ${consumer_profile_arguments})

message(STATUS "${test_name}: building the consumer")
run_step("consumer build" "${CMAKE_COMMAND}" --build "${consumer_build}" ${consumer_config_arguments})

set(consumer_path_file "${consumer_build}/mddlog-consumer-${consumer_config}.txt")
if(NOT EXISTS "${consumer_path_file}")
    message(FATAL_ERROR "${test_name}: program paths were not generated for '${consumer_config}': ${consumer_path_file}")
endif()
file(STRINGS "${consumer_path_file}" consumer_paths)
list(LENGTH consumer_paths program_count)
if(program_count LESS 2)
    message(FATAL_ERROR "${test_name}: expected consumer programs, found '${consumer_paths}'")
endif()
foreach(entry IN LISTS consumer_paths)
    if(NOT entry MATCHES "^([a-z_]+)=(.+)$")
        message(FATAL_ERROR "${test_name}: malformed program entry '${entry}'")
    endif()
    set(program "${CMAKE_MATCH_1}")
    set(executable "${CMAKE_MATCH_2}")
    if(NOT EXISTS "${executable}")
        message(FATAL_ERROR "${test_name}: ${program} was not built (path: '${executable}')")
    endif()
    set(program_arguments)
    if(program STREQUAL "audit_tool")
        set(program_arguments --help)
    endif()
    message(STATUS "${test_name}: running ${program}")
    execute_process(COMMAND "${executable}" ${program_arguments}
        RESULT_VARIABLE run_result OUTPUT_QUIET)
    if(NOT run_result EQUAL 0)
        message(FATAL_ERROR "${test_name}: ${program} exited with ${run_result} (expected 0 - see its assertions)")
    endif()
endforeach()

message(STATUS "${test_name}: OK")
