# Tests for mddlog

include(${CMAKE_SOURCE_DIR}/cmake/MddlogTestDiscovery.cmake)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(mddlog_sha256_oracle spec/Sha256Oracle.cpp)
target_link_libraries(mddlog_sha256_oracle PRIVATE mddlog::mddlog)
add_test(NAME audit.sha256-oracle
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/run-audit-robustness.py
        --sha-worker $<TARGET_FILE:mddlog_sha256_oracle>
        --output ${CMAKE_CURRENT_BINARY_DIR}/audit-sha256 --ephemeral)
set_tests_properties(audit.sha256-oracle PROPERTIES LABELS "integration;audit" TIMEOUT 40)

add_executable(mddlog_context_adapter_consumer consumer/ContextAdapterConsumer.cpp)
target_link_libraries(mddlog_context_adapter_consumer PRIVATE mddlog::mddlog)
add_test(NAME SourceTreeContextConsumer COMMAND mddlog_context_adapter_consumer)
set_tests_properties(SourceTreeContextConsumer PROPERTIES LABELS "build;api")

# The same core-only program also runs against the installed package. Keep it independent of
# SpecLab and of mddlog::mddlog so linking or importing an adapter cannot mask a broken split.
add_executable(mddlog_core_source_consumer consumer/CoreConsumer.cpp)
target_link_libraries(mddlog_core_source_consumer PRIVATE mddlog::core)
if(23 IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
    set_target_properties(mddlog_core_source_consumer PROPERTIES CXX_MODULE_STD ON)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(mddlog_core_source_consumer PRIVATE /experimental:module /std:c++latest)
endif()
add_test(NAME SourceTreeCoreConsumer COMMAND mddlog_core_source_consumer)
set_tests_properties(SourceTreeCoreConsumer PROPERTIES LABELS "build;core")

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

include(${PROJECT_SOURCE_DIR}/cmake/GovernedChecks.cmake)

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
    spec/GovernedRecordSpec.cpp
    spec/RingLogSpec.cpp
    spec/AuditRingSpec.cpp
    spec/AuditRobustnessSpec.cpp
    spec/AuditGeneratedRecoverySpec.cpp
    spec/AuditScenarioSpec.cpp
    spec/AuditDrainSpec.cpp
    spec/AuditResourceSpec.cpp
    spec/AuditServiceSpec.cpp
    spec/AuditCanonicalSpec.cpp
    spec/AuditAnchorSpec.cpp
    spec/AuditStorageSpec.cpp
    spec/AuditLedgerSpec.cpp
    spec/AuditRestartSpec.cpp
    spec/AuditRetentionSpec.cpp
    spec/AuditEndToEndSpec.cpp
    spec/RingLogConcurrencySpec.cpp
    spec/RingDrainSpec.cpp
    spec/SinkRegistrySpec.cpp
    spec/TransportConsumerSpec.cpp
    spec/TextLoggerSpec.cpp
    spec/DiagnosticContextSpec.cpp
    spec/DiagnosticBindingSpec.cpp
    spec/AuditContextSpec.cpp
    spec/ContextIntegrationSpec.cpp
    spec/VersionSpec.cpp
)

if(MDDLOG_BUILD_FILE_STORAGE)
    add_executable(mddlog_witness_deployment spec/WitnessDeployment.cpp)
    target_link_libraries(mddlog_witness_deployment PRIVATE mddlog::mddlog)
    if(MDDLOG_BUILD_EXAMPLES)
        add_test(NAME witness.deployment COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/run-witness-deployment.py
            --worker $<TARGET_FILE:mddlog_witness_deployment>
            --service $<TARGET_FILE:mddlog_witness>)
        set_tests_properties(witness.deployment PROPERTIES LABELS "audit;integration" TIMEOUT 75 SKIP_RETURN_CODE 77)
    endif()
    add_executable(mddlog_file_storage_campaign spec/FileStorageCampaign.cpp)
    target_link_libraries(mddlog_file_storage_campaign PRIVATE mddlog::mddlog)
    add_test(NAME file-storage.campaign
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/run-file-storage-campaign.py
            --worker $<TARGET_FILE:mddlog_file_storage_campaign>
            --output ${CMAKE_CURRENT_BINARY_DIR}/file-storage-campaign)
    set_tests_properties(file-storage.campaign PROPERTIES LABELS "integration;storage" TIMEOUT 120)
    add_executable(mddlog_file_storage_consumer consumer/FileStorageConsumer.cpp)
    target_link_libraries(mddlog_file_storage_consumer PRIVATE mddlog::mddlog)
    add_test(NAME SourceTreeFileStorageConsumer COMMAND mddlog_file_storage_consumer)
    set_tests_properties(SourceTreeFileStorageConsumer PROPERTIES LABELS "build;storage")
    target_sources(mddlog_specs PRIVATE spec/FileStorageSpec.cpp spec/FileRetainedPositionSpec.cpp spec/WitnessSpec.cpp)
endif()

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

if(MDDLOG_BUILD_FUZZERS)
    add_executable(mddlog_audit_readers_fuzz spec/AuditReadersFuzz.cpp)
    target_link_libraries(mddlog_audit_readers_fuzz PRIVATE mddlog::mddlog)
    target_compile_options(mddlog_audit_readers_fuzz PRIVATE -fsanitize=fuzzer-no-link
        "-fprofile-instr-generate=${_mddlog_fuzz_default_profile}" -fcoverage-mapping)
    target_link_options(mddlog_audit_readers_fuzz PRIVATE -fsanitize=fuzzer)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # The distribution's Clang runtime uses libstdc++; only libFuzzer's C ABI crosses
        # this boundary. mddlog and its module importers continue to use libc++.
        target_link_libraries(mddlog_audit_readers_fuzz PRIVATE "-Wl,-lstdc++")
    endif()
endif()

# Run the executable before/after usage study when examples are enabled.
if(TARGET mddlog_contextual_usage)
    add_test(NAME examples.contextualUsage COMMAND mddlog_contextual_usage)
    set_tests_properties(examples.contextualUsage PROPERTIES LABELS "examples;api")
endif()

if(TARGET mddlog_contextual_inventory)
    foreach(scenario equivalence refusals lifecycle)
        add_test(NAME examples.contextualInventory.${scenario} COMMAND mddlog_contextual_inventory ${scenario})
        set_tests_properties(examples.contextualInventory.${scenario} PROPERTIES LABELS "examples;api;integration" TIMEOUT 20)
    endforeach()
endif()

if(TARGET mddlog_file_audit)
    add_test(NAME examples.fileAudit.lifecycle
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/FileAuditLifecycle.py
            $<TARGET_FILE:mddlog_file_audit>)
    set_tests_properties(examples.fileAudit.lifecycle PROPERTIES LABELS "examples;storage;integration" TIMEOUT 60)
endif()

if(TARGET mddlog_audit_application)
    add_test(NAME audit.application COMMAND mddlog_audit_application)
    set_tests_properties(audit.application PROPERTIES LABELS "integration")
endif()

# Reproducible corpus and resource measurements (#117), also usable against v0.2.
add_executable(mddlog_resource_bench bench/AuditResourcesBench.cpp)
target_link_libraries(mddlog_resource_bench PRIVATE mddlog::mddlog)
set_target_properties(mddlog_resource_bench PROPERTIES CXX_MODULE_STD ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(mddlog_host_resource_bench bench/AuditHostResourcesBench.cpp)
    target_link_libraries(mddlog_host_resource_bench PRIVATE mddlog::mddlog)
    set_target_properties(mddlog_host_resource_bench PROPERTIES CXX_MODULE_STD ON)
endif()
add_executable(mddlog_service_resource_bench bench/AuditServiceResourcesBench.cpp)
target_link_libraries(mddlog_service_resource_bench PRIVATE mddlog::mddlog)
set_target_properties(mddlog_service_resource_bench PROPERTIES CXX_MODULE_STD ON)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME audit.service.resourceProfile COMMAND mddlog_service_resource_bench)
    set_tests_properties(audit.service.resourceProfile PROPERTIES LABELS "integration" TIMEOUT 60)
endif()
# Keep replacement allocation functions out of the shared scenario runner.
add_executable(mddlog_retention_allocation spec/AuditRetentionAllocation.cpp)
target_link_libraries(mddlog_retention_allocation PRIVATE mddlog::mddlog)
set_target_properties(mddlog_retention_allocation PROPERTIES CXX_MODULE_STD ON)
add_test(NAME audit.retention.allocation COMMAND mddlog_retention_allocation)
set_tests_properties(audit.retention.allocation PROPERTIES LABELS "storage;integration" TIMEOUT 30)

# Diagnostic overload and lifecycle scenarios (#118), independent of application code.
add_executable(mddlog_bounded_diagnostic_consumer consumer/BoundedDiagnosticConsumer.cpp)
target_link_libraries(mddlog_bounded_diagnostic_consumer PRIVATE mddlog::mddlog)
foreach(scenario IN ITEMS overload failures-sync failures-async callbacks-sync callbacks-async concurrent order active-flush configuration console global sync-refusals)
    add_test(NAME diagnostic.bounded.${scenario} COMMAND mddlog_bounded_diagnostic_consumer ${scenario})
    set_tests_properties(diagnostic.bounded.${scenario} PROPERTIES LABELS "diagnostic;concurrency" TIMEOUT 30)
endforeach()

add_executable(mddlog_diagnostic_memory_consumer consumer/DiagnosticMemoryConsumer.cpp)
target_link_libraries(mddlog_diagnostic_memory_consumer PRIVATE mddlog::mddlog)
add_test(NAME diagnostic.bounded.memory COMMAND mddlog_diagnostic_memory_consumer)
set_tests_properties(diagnostic.bounded.memory PROPERTIES LABELS "diagnostic;resources" TIMEOUT 30)
