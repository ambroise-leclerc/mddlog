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
#              -P TestInstallConsumer.cmake
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

file(REMOVE_RECURSE "${install_prefix}" "${consumer_src}" "${consumer_build}")

message(STATUS "${test_name}: installing to ${install_prefix}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${install_prefix}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "cmake --install failed (${install_result}):\n${install_output}\n${install_error}")
endif()

# A minimal external project - deliberately not part of this repository's own CMake build graph,
# so it can only see mddlog through find_package(), exactly as a real downstream consumer would.
if(CONSUMER_KIND STREQUAL "full")
file(WRITE "${consumer_src}/main.cpp" "\
import std;\n\
import mddlog;\n\
import mddlog.core.ring;\n\
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

set(consumer_target "consumer")
# Only the full target: its declared dependency must bring mddlog::core and the core modules.
set(consumer_link_target "mddlog::mddlog")
set(consumer_source "main.cpp")
else()
    # Reuse the exact program exercised in-tree. Only mddlog::core may be linked here.
    file(MAKE_DIRECTORY "${consumer_src}")
    file(COPY_FILE "${SOURCE_DIR}/tests/consumer/CoreConsumer.cpp" "${consumer_src}/main_core.cpp")
    set(consumer_target "consumer_core")
    set(consumer_link_target "mddlog::core")
    set(consumer_source "main_core.cpp")
endif()

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
foreach(required_target IN ITEMS ${consumer_link_target})\n\
    if(NOT TARGET \${required_target})\n\
        message(FATAL_ERROR \"Installed package does not provide \${required_target}\")\n\
    endif()\n\
endforeach()\n\
add_executable(${consumer_target} ${consumer_source})\n\
target_link_libraries(${consumer_target} PRIVATE ${consumer_link_target})\n\
if(23 IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)\n\
    set_target_properties(${consumer_target} PROPERTIES CXX_MODULE_STD ON)\n\
endif()\n\
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
if(DEFINED BUILD_TYPE AND NOT BUILD_TYPE STREQUAL "")
    # On MSVC in particular, CMAKE_BUILD_TYPE selects the runtime library (/MD vs /MT, Release vs
    # Debug). A consumer configured without it links against a different default runtime than the
    # installed mddlog.lib was built with and fails with LNK4098/LNK1319.
    list(APPEND consumer_toolchain_arguments "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}")
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
    COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "Consumer build failed (${build_result}):\n${build_output}\n${build_error}")
endif()

set(consumer_executable "${consumer_build}/${consumer_target}")
if(WIN32)
    string(APPEND consumer_executable ".exe")
endif()
message(STATUS "${test_name}: running ${consumer_target} executable")
execute_process(
    COMMAND "${consumer_executable}"
    RESULT_VARIABLE run_result
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "${consumer_target} exited with ${run_result} (expected 0 - see ${consumer_source}'s assertions)")
endif()

message(STATUS "${test_name}: OK")
