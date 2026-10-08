# TestPackageRefusals.cmake
#
# The installed package must refuse what it cannot serve at configure time, with a message that
# names the remedy (#121, ADR-007): a version outside SameMinorVersion/SameMajorVersion, an unknown
# component, a generator without C++ module support and a compiler other than the one mddlog was
# built with. Each case configures a minimal throwaway consumer and expects failure plus the text.
#
# Run via: cmake -D BUILD_DIR=... -D CXX_COMPILER=... -D GENERATOR=... -D EXPECTED_VERSION=...
#              [-D CONFIG=...] [-D CXX_FLAGS=...] [-D STDLIB_MODULES_JSON=...]
#              [-D OTHER_CXX_COMPILER=<a different compiler, for the toolchain case>]
#              -P TestPackageRefusals.cmake

foreach(required_var BUILD_DIR CXX_COMPILER GENERATOR EXPECTED_VERSION)
    if(NOT DEFINED ${required_var})
        message(FATAL_ERROR "TestPackageRefusals.cmake: ${required_var} must be set with -D")
    endif()
endforeach()

set(work_dir "${BUILD_DIR}/_test_package_refusals")
set(install_prefix "${work_dir}/prefix")
file(REMOVE_RECURSE "${work_dir}")
set(config_arguments)
if(DEFINED CONFIG AND NOT CONFIG STREQUAL "")
    set(config_arguments --config "${CONFIG}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${install_prefix}" ${config_arguments}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "cmake --install failed (${result}):\n${output}\n${error}")
endif()

string(REGEX MATCH "^([0-9]+)\\.([0-9]+)" _ "${EXPECTED_VERSION}")
math(EXPR other_minor "${CMAKE_MATCH_2} + 1")
set(other_version "${CMAKE_MATCH_1}.${other_minor}")

set(toolchain_arguments)
if(DEFINED CXX_FLAGS AND NOT CXX_FLAGS STREQUAL "")
    list(APPEND toolchain_arguments "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}")
endif()
if(DEFINED STDLIB_MODULES_JSON AND NOT STDLIB_MODULES_JSON STREQUAL "")
    list(APPEND toolchain_arguments "-DCMAKE_CXX_STDLIB_MODULES_JSON=${STDLIB_MODULES_JSON}")
endif()

# name|generator|compiler|find_package arguments|expected text
set(cases
    "newer-minor|${GENERATOR}|${CXX_COMPILER}|${other_version} CONFIG REQUIRED|${other_version}"
    "unknown-component|${GENERATOR}|${CXX_COMPILER}|CONFIG REQUIRED COMPONENTS core no_such_part|Unknown mddlog component 'no_such_part'")
if(NOT CMAKE_HOST_WIN32)
    list(APPEND cases "makefiles|Unix Makefiles|${CXX_COMPILER}|CONFIG REQUIRED|need a Ninja generator")
endif()
if(DEFINED OTHER_CXX_COMPILER AND NOT OTHER_CXX_COMPILER STREQUAL "")
    list(APPEND cases "other-compiler|${GENERATOR}|${OTHER_CXX_COMPILER}|CONFIG REQUIRED|build mddlog from source with the consumer's toolchain")
endif()

foreach(case IN LISTS cases)
    string(REPLACE "|" ";" fields "${case}")
    list(GET fields 0 name)
    list(GET fields 1 generator)
    list(GET fields 2 compiler)
    list(GET fields 3 arguments)
    list(GET fields 4 expected)
    set(source "${work_dir}/${name}")
    file(WRITE "${source}/CMakeLists.txt" "\
cmake_minimum_required(VERSION 4.0.0)\n\
if(CMAKE_VERSION VERSION_GREATER_EQUAL \"4.3\")\n\
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD \"451f2fe2-a8a2-47c3-bc32-94786d8fc91b\")\n\
else()\n\
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD \"d0edc3af-4c50-42ea-a356-e2862fe7a444\")\n\
endif()\n\
set(CMAKE_CXX_STANDARD 23)\n\
set(CMAKE_CXX_EXTENSIONS OFF)\n\
project(MddlogRefusal${name} LANGUAGES CXX)\n\
find_package(mddlog ${arguments})\n")
    # The other compiler keeps its own default standard library: only its identity matters here.
    set(case_toolchain ${toolchain_arguments})
    if(name STREQUAL "other-compiler")
        set(case_toolchain)
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${source}" -B "${source}/build" -G "${generator}"
                "-DCMAKE_CXX_COMPILER=${compiler}" ${case_toolchain} "-DCMAKE_PREFIX_PATH=${install_prefix}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    string(CONCAT combined "${output}" "${error}")
    # CMake wraps long messages; compare with whitespace collapsed.
    string(REGEX REPLACE "[ \t\r\n]+" " " combined "${combined}")
    if(result EQUAL 0)
        message(FATAL_ERROR "${name}: the consumer configured successfully; a refusal was expected.")
    endif()
    string(FIND "${combined}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${name}: refused without the expected text '${expected}':\n${combined}")
    endif()
    message(STATUS "${name}: refused as expected")
endforeach()
message(STATUS "TestPackageRefusals: OK")
