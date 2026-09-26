# TestInstallConsumer.cmake
#
# Proves the install tree actually works, not just that `cmake --install` exits 0: installs to a
# scratch prefix, then configures, builds, and RUNS a tiny external consumer project against that
# prefix via find_package(mddlog CONFIG REQUIRED). The consumer program asserts a real value from
# the installed module, so a successful run means import std + the installed FILE_SET CXX_MODULES
# are both genuinely usable from outside this source tree, not just present on disk.
#
# Run via: cmake -D BUILD_DIR=... -D CXX_COMPILER=... -D GENERATOR=...
#              -D EXPECTED_VERSION=... -D SOURCE_DIR=... -D CONSUMER_KIND=full|core
#              [-D CONFIG=<configuration under test>] -P TestInstallConsumer.cmake
# (see the two install-consumer add_test() calls in the top-level CMakeLists.txt)

foreach(required_var BUILD_DIR CXX_COMPILER GENERATOR EXPECTED_VERSION SOURCE_DIR CONSUMER_KIND)
    if(NOT DEFINED ${required_var})
        message(FATAL_ERROR "TestInstallConsumer.cmake: ${required_var} must be set with -D")
    endif()
endforeach()
if(NOT CONSUMER_KIND MATCHES "^(full|core)$")
    message(FATAL_ERROR "CONSUMER_KIND must be full or core")
endif()

set(install_prefix "${BUILD_DIR}/_test_install_${CONSUMER_KIND}_prefix")
set(consumer_src "${BUILD_DIR}/_test_install_${CONSUMER_KIND}_src")
set(consumer_build "${BUILD_DIR}/_test_install_${CONSUMER_KIND}_build")
set(test_name "InstallTreeConsumer")
if(CONSUMER_KIND STREQUAL "core")
    set(test_name "InstallTreeCoreConsumer")
endif()

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

file(REMOVE_RECURSE "${install_prefix}" "${consumer_src}" "${consumer_build}")

message(STATUS "${test_name}: installing to ${install_prefix}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${install_prefix}" ${config_arguments}
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "cmake --install failed (${install_result}):\n${install_output}\n${install_error}")
endif()

# A minimal external project - deliberately not part of this repository's own CMake build graph,
# so it can only see mddlog through find_package(), exactly as a real downstream consumer would.
# Each consumer executable is "target|source|linked targets" (links separated by ',').
if(CONSUMER_KIND STREQUAL "full")
# Imports only the umbrella and links only mddlog::mddlog, so the installed full target must bring
# mddlog::core, its modules and its library through its own declared dependency.
file(WRITE "${consumer_src}/main.cpp" "\
import std;\n\
import mddlog;\n\
\n\
int main() {\n\
    // A real assertion, not just \"it links\": confirms the installed module is actually usable\n\
    // and its constexpr data survived installation intact.\n\
    if (mddlog::getVersion() != \"${EXPECTED_VERSION}\") {\n\
        return 1;\n\
    }\n\
    if (!mddlog::isMedicalComplianceEnabled()) {\n\
        return 2;\n\
    }\n\
    auto logger = mddlog::SimpleLogger(\"InstallConsumer\", false);\n\
    logger.addSink(mddlog::createConsoleSink(false, false));\n\
    logger.info(\"install consumer smoke test\");\n\
    logger.flush();\n\
    return 0;\n\
}\n\
")
# A translation unit that directly imports a governed module must also link the target providing
# it: CMake 4.1 with GCC does not place transitively linked modules in its module mapper.
file(WRITE "${consumer_src}/main_ring.cpp" "\
import std;\n\
import mddlog;\n\
import mddlog.core.ring;\n\
\n\
int main() {\n\
    mddlog::core::RingLog<1> ring;\n\
    mddlog::RingSinkAdapter adapter;\n\
    adapter.addRing(ring);\n\
    if (ring.tryWrite({.time = mddlog::core::RawTime::unavailable(), .message = \"installed bridge\"}).admission() !=\n\
        mddlog::core::Admission::Written || adapter.drainOnce() != 1) {\n\
        return 3;\n\
    }\n\
    return 0;\n\
}\n\
")
set(consumer_executables "consumer|main.cpp|mddlog::mddlog" "consumer_ring|main_ring.cpp|mddlog::mddlog,mddlog::core")
else()
    # Reuse the exact program exercised in-tree. Only mddlog::core may be linked here.
    file(MAKE_DIRECTORY "${consumer_src}")
    file(COPY_FILE "${SOURCE_DIR}/tests/consumer/CoreConsumer.cpp" "${consumer_src}/main_core.cpp")
    set(consumer_executables "consumer_core|main_core.cpp|mddlog::core")
endif()

set(consumer_required_targets)
set(consumer_target_definitions)
# The consumer records each executable's real path per configuration, so no generator-specific
# output layout (configuration subdirectories, .exe suffix) is guessed here.
set(consumer_path_content)
foreach(consumer_executable_spec IN LISTS consumer_executables)
    string(REPLACE "|" ";" consumer_fields "${consumer_executable_spec}")
    list(GET consumer_fields 0 consumer_target)
    list(GET consumer_fields 1 consumer_source)
    list(GET consumer_fields 2 consumer_links)
    string(REPLACE "," " " consumer_links "${consumer_links}")
    string(APPEND consumer_target_definitions "\
add_executable(${consumer_target} ${consumer_source})\n\
target_link_libraries(${consumer_target} PRIVATE ${consumer_links})\n\
if(23 IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)\n\
    set_target_properties(${consumer_target} PROPERTIES CXX_MODULE_STD ON)\n\
endif()\n\
")
    string(APPEND consumer_path_content "${consumer_target}=$<TARGET_FILE:${consumer_target}>\\n")
    string(REPLACE " " ";" consumer_link_list "${consumer_links}")
    list(APPEND consumer_required_targets ${consumer_link_list})
endforeach()
list(REMOVE_DUPLICATES consumer_required_targets)
list(JOIN consumer_required_targets " " consumer_required_targets)

file(WRITE "${consumer_src}/CMakeLists.txt" "\
cmake_minimum_required(VERSION 4.0.0)\n\
if(CMAKE_VERSION VERSION_GREATER_EQUAL \"4.4\")\n\
    message(FATAL_ERROR \"This consumer has not qualified CMake's newer import std gate\")\n\
endif()\n\
if(CMAKE_VERSION VERSION_GREATER_EQUAL \"4.3\")\n\
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD \"451f2fe2-a8a2-47c3-bc32-94786d8fc91b\")\n\
else()\n\
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD \"d0edc3af-4c50-42ea-a356-e2862fe7a444\")\n\
endif()\n\
set(CMAKE_CXX_STANDARD 23)\n\
set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\
set(CMAKE_CXX_EXTENSIONS OFF)\n\
project(MddlogInstallConsumer LANGUAGES CXX)\n\
set(CMAKE_CXX_SCAN_FOR_MODULES ON)\n\
if(23 IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)\n\
    set(CMAKE_CXX_MODULE_STD ON)\n\
endif()\n\
find_package(mddlog CONFIG REQUIRED)\n\
foreach(required_target IN ITEMS ${consumer_required_targets})\n\
    if(NOT TARGET \${required_target})\n\
        message(FATAL_ERROR \"Installed package does not provide \${required_target}\")\n\
    endif()\n\
endforeach()\n\
${consumer_target_definitions}\
file(GENERATE OUTPUT \"\${CMAKE_BINARY_DIR}/mddlog-consumer-$<CONFIG>.txt\" CONTENT \"${consumer_path_content}\")\n\
")

message(STATUS "${test_name}: configuring consumer project")
set(consumer_toolchain_arguments)
if(DEFINED AR AND NOT AR STREQUAL "")
    list(APPEND consumer_toolchain_arguments "-DCMAKE_AR=${AR}")
endif()
if(DEFINED RANLIB AND NOT RANLIB STREQUAL "")
    list(APPEND consumer_toolchain_arguments "-DCMAKE_RANLIB=${RANLIB}")
endif()
if(DEFINED STDLIB_MODULES_JSON AND NOT STDLIB_MODULES_JSON STREQUAL "")
    list(APPEND consumer_toolchain_arguments
        "-DCMAKE_CXX_STDLIB_MODULES_JSON=${STDLIB_MODULES_JSON}")
endif()
if(DEFINED OSX_SYSROOT AND NOT OSX_SYSROOT STREQUAL "")
    list(APPEND consumer_toolchain_arguments "-DCMAKE_OSX_SYSROOT=${OSX_SYSROOT}")
endif()
if(DEFINED CXX_FLAGS AND NOT CXX_FLAGS STREQUAL "")
    # Carries -stdlib=libc++ where the toolchain sets it. Without this the consumer builds against
    # the compiler's default standard library while linking an mddlog built against another one.
    list(APPEND consumer_toolchain_arguments "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}")
endif()
if(NOT config STREQUAL "")
    # On MSVC in particular, the configuration selects the runtime library (/MD vs /MT, Release vs
    # Debug). A consumer configured without it links against a different default runtime than the
    # installed mddlog.lib was built with and fails with LNK4098/LNK1319. A multi-config consumer
    # ignores CMAKE_BUILD_TYPE and receives the same configuration through --config below.
    list(APPEND consumer_toolchain_arguments "-DCMAKE_BUILD_TYPE=${config}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -B "${consumer_build}" -S "${consumer_src}"
            -G "${GENERATOR}"
            -DCMAKE_CXX_COMPILER=${CXX_COMPILER}
            ${consumer_toolchain_arguments}
            -DCMAKE_PREFIX_PATH=${install_prefix}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "Consumer configure failed (${configure_result}):\n${configure_output}\n${configure_error}")
endif()

message(STATUS "${test_name}: building consumer project")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" ${config_arguments}
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "Consumer build failed (${build_result}):\n${build_output}\n${build_error}")
endif()

set(consumer_path_file "${consumer_build}/mddlog-consumer-${config}.txt")
if(NOT EXISTS "${consumer_path_file}")
    message(FATAL_ERROR "Consumer executable paths were not generated for configuration '${config}': ${consumer_path_file}")
endif()
file(STRINGS "${consumer_path_file}" consumer_paths)
foreach(consumer_executable_spec IN LISTS consumer_executables)
    string(REPLACE "|" ";" consumer_fields "${consumer_executable_spec}")
    list(GET consumer_fields 0 consumer_target)
    list(GET consumer_fields 1 consumer_source)
    set(consumer_executable "")
    foreach(consumer_path_entry IN LISTS consumer_paths)
        if(consumer_path_entry MATCHES "^${consumer_target}=(.+)$")
            set(consumer_executable "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(consumer_executable STREQUAL "" OR NOT EXISTS "${consumer_executable}")
        message(FATAL_ERROR "${consumer_target} was not built for configuration '${config}' (path: '${consumer_executable}')")
    endif()
    message(STATUS "${test_name}: running ${consumer_target} executable")
    execute_process(
        COMMAND "${consumer_executable}"
        RESULT_VARIABLE run_result
    )
    if(NOT run_result EQUAL 0)
        message(FATAL_ERROR "${consumer_target} exited with ${run_result} (expected 0 - see ${consumer_source}'s assertions)")
    endif()
endforeach()

message(STATUS "${test_name}: OK")
