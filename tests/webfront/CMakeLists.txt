# Acceptance suite from the immutable ADR-003 / #66 baseline. Download only: WebFront's
# default build, toolchain floor and header-only implementation are not configured here.
CPMAddPackage(
    NAME WebFrontBaseline
    GITHUB_REPOSITORY ambroise-leclerc/WebFront
    GIT_TAG 7be626ccfbb50524c9c03296e6c84555ea7d2c7c
    DOWNLOAD_ONLY YES
)
CPMAddPackage(
    NAME Catch2
    GITHUB_REPOSITORY catchorg/Catch2
    # v3.8.1
    GIT_TAG 56809e5282f104c5c8b570e7c2996cdc352d94f1
    OPTIONS "CATCH_BUILD_TESTING OFF" "CATCH_INSTALL_DOCS OFF" "CATCH_INSTALL_EXTRAS OFF"
)

# WebFront owns this header/TU in the proposed integration. Keep this reference bridge test-only;
# mddlog installs no header-only API and no WebFront-specific production target.
add_library(mddlog_webfront_test_bridge STATIC Logger.cpp)
target_include_directories(mddlog_webfront_test_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mddlog_webfront_test_bridge PRIVATE mddlog::mddlog mddlog_warnings)

# Byte-for-byte copy with a local path; no assertions, LLVM guards or sections are rewritten.
configure_file(${WebFrontBaseline_SOURCE_DIR}/test/LoggerTests.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/LoggerTests.cpp COPYONLY)
add_executable(mddlog_webfront_acceptance ${CMAKE_CURRENT_BINARY_DIR}/LoggerTests.cpp)
target_link_libraries(mddlog_webfront_acceptance PRIVATE mddlog_webfront_test_bridge Catch2::Catch2WithMain)
add_test(NAME webfront.baseline.LoggerTests COMMAND mddlog_webfront_acceptance)
set_tests_properties(webfront.baseline.LoggerTests PROPERTIES LABELS "unit;webfront")

target_link_libraries(mddlog_specs PRIVATE mddlog_webfront_test_bridge)

# Actual WebFront-owned opt-in integration (#71), distinct from the #66 reference baseline.
# An immutable revision lets every supported CI leg build the same application and adapter.
CPMAddPackage(
    NAME WebFrontIntegration
    GITHUB_REPOSITORY ambroise-leclerc/WebFront
    GIT_TAG b2c2dd6f2f03b826337c5d0f2a06c2445cda2f18
    DOWNLOAD_ONLY YES
)
foreach(kind IN ITEMS source installed)
    add_test(NAME webfront.integration.${kind}
        COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DWEBFRONT_SOURCE_DIR=${WebFrontIntegration_SOURCE_DIR}"
            "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DAR=${CMAKE_AR}" "-DRANLIB=${CMAKE_RANLIB}"
            "-DSTDLIB_MODULES_JSON=${CMAKE_CXX_STDLIB_MODULES_JSON}"
            "-DOSX_SYSROOT=${CMAKE_OSX_SYSROOT}"
            "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
            "-DMSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}"
            "-DCONFIG=$<CONFIG>" "-DGENERATOR=${CMAKE_GENERATOR}" "-DKIND=${kind}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/TestIntegration.cmake")
    set_tests_properties(webfront.integration.${kind} PROPERTIES
        LABELS "build;webfront;integration" TIMEOUT 900 RESOURCE_LOCK mddlog_install_tree)
endforeach()
