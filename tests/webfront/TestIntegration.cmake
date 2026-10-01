# Build the actual WebFront project against the source tree or an installed package.
# Keep this separate from the reference bridge's original LoggerTests acceptance suite.
foreach(required IN ITEMS BUILD_DIR SOURCE_DIR WEBFRONT_SOURCE_DIR CXX_COMPILER GENERATOR KIND)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "TestIntegration.cmake: missing ${required}")
    endif()
endforeach()
if(NOT KIND MATCHES "^(source|installed)$")
    message(FATAL_ERROR "KIND must be source or installed")
endif()
set(scratch "${BUILD_DIR}/_webfront_${KIND}")
set(config_args)
if(CONFIG)
    list(APPEND config_args --config "${CONFIG}")
endif()

# Preserve successful build trees for incremental reruns; each kind has its own prefix/build.
set(dependency_args)
if(KIND STREQUAL "source")
    list(APPEND dependency_args "-DWEBFRONT_MDDLOG_SOURCE_DIR=${SOURCE_DIR}")
else()
    message(STATUS "WebFront ${KIND}: installing mddlog")
    execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}"
        --prefix "${scratch}/prefix" ${config_args} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "WebFront ${KIND}: install failed (${result})")
    endif()
    list(APPEND dependency_args "-DCMAKE_PREFIX_PATH=${scratch}/prefix")
endif()

set(toolchain_args "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
foreach(pair IN ITEMS "AR|CMAKE_AR" "RANLIB|CMAKE_RANLIB"
        "STDLIB_MODULES_JSON|CMAKE_CXX_STDLIB_MODULES_JSON" "OSX_SYSROOT|CMAKE_OSX_SYSROOT"
        "CXX_FLAGS|CMAKE_CXX_FLAGS" "CONFIG|CMAKE_BUILD_TYPE"
        "MSVC_RUNTIME_LIBRARY|CMAKE_MSVC_RUNTIME_LIBRARY")
    string(REPLACE "|" ";" fields "${pair}")
    list(GET fields 0 input)
    list(GET fields 1 setting)
    if(DEFINED ${input} AND NOT "${${input}}" STREQUAL "")
        list(APPEND toolchain_args "-D${setting}=${${input}}")
    endif()
endforeach()

message(STATUS "WebFront ${KIND}: configuration")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WEBFRONT_SOURCE_DIR}" -B "${scratch}/build"
    -G "${GENERATOR}" ${toolchain_args} ${dependency_args}
    -DWEBFRONT_USE_MDDLOG=ON -DWEBFRONT_EMBED_CEF=OFF -DENABLE_TESTING=ON
    -DENABLE_CACHE=OFF "-DCPM_SOURCE_CACHE=${BUILD_DIR}/_webfront_dependency_cache"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "WebFront ${KIND}: configuration failed (${result})")
endif()

message(STATUS "WebFront ${KIND}: compilation and static adapter linkage")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${scratch}/build"
    --target WebFront_mddlog --parallel 2 ${config_args} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "WebFront ${KIND}: adapter build failed (${result})")
endif()
message(STATUS "WebFront ${KIND}: consumer compilation and executable linkage")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${scratch}/build"
    --parallel 2 ${config_args} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "WebFront ${KIND}: consumer compilation/linkage failed (${result})")
endif()

message(STATUS "WebFront ${KIND}: execution of original LoggerTests and facade regressions")
set(ctest_config_args)
if(CONFIG)
    list(APPEND ctest_config_args -C "${CONFIG}")
endif()
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${scratch}/build"
    --output-on-failure --no-tests=error ${ctest_config_args} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "WebFront ${KIND}: test execution failed (${result})")
endif()
message(STATUS "WebFront ${KIND}: configuration, compilation, linkage and execution passed")
