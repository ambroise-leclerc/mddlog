option(ENABLE_CACHE "Enable a compiler cache if available" ON)
set(CACHE_OPTION
    "ccache"
    CACHE STRING "Compiler cache to be used")
set(CACHE_BINARY "" CACHE FILEPATH
    "Compiler cache executable override; empty searches for CACHE_OPTION")
set(CACHE_OPTION_VALUES "ccache" "sccache")
set_property(CACHE CACHE_OPTION PROPERTY STRINGS ${CACHE_OPTION_VALUES})
list(FIND CACHE_OPTION_VALUES "${CACHE_OPTION}" CACHE_OPTION_INDEX)

# These cache entries are the explicit contract consumed by tests/CMakeLists.txt. Reset them on
# every configure so normal early-return paths cannot leave stale capability state behind.
set(MDDLOG_CACHE_BINARY "" CACHE INTERNAL "Resolved compiler cache executable" FORCE)
set(MDDLOG_CACHE_KIND "disabled" CACHE INTERNAL "Resolved compiler cache kind" FORCE)
set(MDDLOG_CACHE_CAN_HASH_BMIS OFF CACHE INTERNAL
    "Whether the compiler cache accepts per-compilation BMI inputs" FORCE)

if(NOT ENABLE_CACHE)
    return()
endif()

if(CACHE_OPTION_INDEX EQUAL -1)
    message(STATUS
        "Using custom compiler cache system: '${CACHE_OPTION}', explicitly supported entries are ${CACHE_OPTION_VALUES}")
endif()

if(CACHE_BINARY)
    if(NOT EXISTS "${CACHE_BINARY}")
        message(FATAL_ERROR
            "CACHE_BINARY was explicitly set to '${CACHE_BINARY}', which does not exist. Fix the "
            "path, or clear CACHE_BINARY to search for '${CACHE_OPTION}' automatically.")
    endif()
    set(mddlog_resolved_cache_binary "${CACHE_BINARY}")
else()
    find_program(mddlog_resolved_cache_binary NAMES "${CACHE_OPTION}" NO_CACHE)
endif()
if(NOT mddlog_resolved_cache_binary)
    message(WARNING "${CACHE_OPTION} is enabled but was not found. Not using it")
    return()
endif()
set(MDDLOG_CACHE_BINARY "${mddlog_resolved_cache_binary}"
    CACHE INTERNAL "Resolved compiler cache executable" FORCE)

# Ccache still does not understand C++20 named-module dependency state. In particular, neither
# depend_mode nor sloppiness=modules adds imported BMIs to an object's key. The launcher below
# therefore compiles module providers directly and, on Clang, hashes every imported BMI before
# caching a consumer. Other named-module command formats fail closed to a direct compilation;
# ordinary translation units remain eligible for the selected cache.
set(MDDLOG_CACHE_KIND "other" CACHE INTERNAL "Resolved compiler cache kind" FORCE)
execute_process(
    COMMAND "${MDDLOG_CACHE_BINARY}" --version
    RESULT_VARIABLE mddlog_cache_version_result
    OUTPUT_VARIABLE mddlog_cache_version_stdout
    ERROR_VARIABLE mddlog_cache_version_stderr
)
string(CONCAT mddlog_cache_version_banner
    "${mddlog_cache_version_stdout}" "\n" "${mddlog_cache_version_stderr}")
if(mddlog_cache_version_result EQUAL 0 AND
   mddlog_cache_version_banner MATCHES "(^|\n)ccache version ([0-9]+(\\.[0-9]+)+)")
    set(MDDLOG_CACHE_KIND "ccache" CACHE INTERNAL "Resolved compiler cache kind" FORCE)
    set(mddlog_cache_version "${CMAKE_MATCH_2}")
    # Per-compilation configuration, used for extra_files_to_hash, arrived in ccache 4.8.
    if(mddlog_cache_version VERSION_GREATER_EQUAL "4.8")
        set(MDDLOG_CACHE_CAN_HASH_BMIS ON CACHE INTERNAL
            "Whether the compiler cache accepts per-compilation BMI inputs" FORCE)
    endif()
endif()

set(CMAKE_CXX_COMPILER_LAUNCHER
    "${CMAKE_COMMAND}"
    "-DMDDLOG_CACHE_BINARY=${MDDLOG_CACHE_BINARY}"
    "-DMDDLOG_CACHE_KIND=${MDDLOG_CACHE_KIND}"
    "-DMDDLOG_CACHE_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}"
    "-DMDDLOG_CACHE_CAN_HASH_BMIS=${MDDLOG_CACHE_CAN_HASH_BMIS}"
    -P "${CMAKE_CURRENT_LIST_DIR}/MddlogCompilerCacheLauncher.cmake"
    --
)

if(MDDLOG_CACHE_KIND STREQUAL "ccache" AND
   CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
   MDDLOG_CACHE_CAN_HASH_BMIS)
    message(STATUS
        "ccache ${mddlog_cache_version} found: module providers compile directly; Clang module consumers hash imported BMIs; other compilations remain cache-eligible")
else()
    message(STATUS
        "${CACHE_OPTION} found: named-module compilations bypass the cache; other compilations remain cache-eligible")
endif()
