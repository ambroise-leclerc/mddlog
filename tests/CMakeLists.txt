# Tests for mddlog

include(${CMAKE_SOURCE_DIR}/cmake/MddlogTestDiscovery.cmake)

# Issue #5's build/cache work: ccache does not model C++20 named-module dependencies. The
# classifier test is always present and pins the fail-closed response-file parser. The integration
# test needs real ccache and Clang's explicit BMI paths, so it only registers on that combination.
add_test(NAME build.cache.classifier
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompilerCacheClassifierTest.cmake)
set_tests_properties(build.cache.classifier PROPERTIES LABELS "build")

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
   MDDLOG_CACHE_KIND STREQUAL "ccache" AND
   MDDLOG_CACHE_CAN_HASH_BMIS)
    add_test(NAME build.cache.namedModuleIntegrity
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DGENERATOR=${CMAKE_GENERATOR}
            -DCACHE_BINARY=${MDDLOG_CACHE_BINARY}
            "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
            "-DEXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompilerCacheIntegrityTest.cmake)
    set_tests_properties(build.cache.namedModuleIntegrity PROPERTIES LABELS "build" TIMEOUT 120)
else()
    message(STATUS
        "build.cache.namedModuleIntegrity not registered: it requires Clang and ccache 4.8+; "
        "build.cache.classifier still verifies fail-closed module-map handling")
endif()

# ADR-001 Decision 6: mddlog-core must import no sink/adapter module (#32). MDDLOG_CORE_MODULE_FILES
# is the same list top-level CMakeLists.txt uses for mddlog-core's FILE_SET, so this test and the
# built target can never drift apart from listing different files.
set(mddlog_core_absolute_module_files)
foreach(module_file IN LISTS MDDLOG_CORE_MODULE_FILES)
    # PROJECT_SOURCE_DIR, not CMAKE_SOURCE_DIR: if mddlog is ever consumed via add_subdirectory()
    # from a parent project, CMAKE_SOURCE_DIR points at that parent's source tree, not mddlog's
    # own - PROJECT_SOURCE_DIR still resolves to mddlog's root (the most recent project() call).
    list(APPEND mddlog_core_absolute_module_files "${PROJECT_SOURCE_DIR}/${module_file}")
endforeach()
add_test(NAME build.core.boundary
    COMMAND ${CMAKE_COMMAND}
        -D "MODULE_FILES=${mddlog_core_absolute_module_files}"
        -P ${PROJECT_SOURCE_DIR}/cmake/CoreBoundaryCheck.cmake)
set_tests_properties(build.core.boundary PROPERTIES LABELS "build")

# ---------------------------------------------------------------------------
# SpecLab BDD specs
# ---------------------------------------------------------------------------
#
# SpecLab (https://github.com/ambroise-leclerc/SpecLab) provides Given/When/Then specifications
# without macros, via `import speclab;`. It is a test-only dependency: no library target links it,
# and mddlog::mddlog's installed interface does not depend on it.
#
# Pinned to a commit, not a tag or branch: a moving ref would let the test framework under a
# medical device library change between two builds of the same mddlog commit. Bump this
# deliberately, in a reviewable diff.
#
# The commit is SpecLab v0.4.0, which provides the scenario registry, the --list-tests / --run=
# runner, Checks, and the speclab::Test<State>(id) overload used throughout these specs.
# cmake/MddlogTestDiscovery.cmake implements the build side of that --list-tests / --run=
# contract, adapted from MduX's per-scenario CTest discovery rather than reimplementing a registry
# SpecLab already provides natively (see MduX issue #361 / SpecLab #28).
#
# GCC 16.1 is this project's floor rather than 16.0 for the same reason: GCC 15 cannot round-trip
# libstdc++'s std module through the second-level BMI that a named module re-exporting parts of
# std - which is exactly what SpecLab's own modules do - requires. SPECLAB_BUILD_TESTS/EXAMPLES are
# OFF and ENABLE_CACHE is OFF so this dependency stays test-only and does not fight this project's
# own compiler-cache configuration.
CPMAddPackage(
    NAME SpecLab
    GITHUB_REPOSITORY ambroise-leclerc/SpecLab
    # SpecLab v0.4.0
    GIT_TAG d85c1f74d95a25b2bf2733316149ba7936ed7df6
    OPTIONS
        "SPECLAB_BUILD_TESTS OFF"
        "SPECLAB_BUILD_EXAMPLES OFF"
        "ENABLE_CACHE OFF"
)

add_executable(mddlog_specs
    spec/SpecMain.cpp
    spec/FilteringSpec.cpp
    spec/RecordFidelitySpec.cpp
    spec/DeliverySpec.cpp
    spec/FlushSpec.cpp
    spec/ConcurrencySpec.cpp
    spec/SinkFailureSpec.cpp
    spec/AuditPolicySpec.cpp
    spec/LogLevelSpec.cpp
    spec/InlineStringSpec.cpp
    spec/WriteResultSpec.cpp
)

target_link_libraries(mddlog_specs PRIVATE mddlog::mddlog speclab::speclab)
target_include_directories(mddlog_specs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(mddlog_specs
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        FOLDER "Tests"
)

if(23 IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
    set_target_properties(mddlog_specs PROPERTIES CXX_MODULE_STD ON)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(mddlog_specs PRIVATE /experimental:module /std:c++latest)
endif()

mddlog_discover_tests(mddlog_specs)
