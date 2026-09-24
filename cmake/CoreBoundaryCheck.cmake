# CoreBoundaryCheck.cmake
#
# ADR-001 Decision 6: mddlog-core (the governed target) must contain no sink or adapter module.
# Decision 6 lists three verification strengths, in decreasing order: a link/import
# dependency-graph check (the top-level CMakeLists.txt configure-time assertion right after
# mddlog-core's target_link_libraries), an object scan over the compiled governed target (not yet
# implemented), and a source-level check for forbidden constructs - this script is that last,
# weakest but simplest one, run as part of `ctest -L build`.
#
# It greps every module file registered under mddlog-core's FILE_SET for an import of a
# mddlog.sinks.* or mddlog.adapter.* module. Nothing here inspects transitive imports (a
# source-level check cannot see through `import std;` or a governed module importing another
# governed one), so it is a cheap first guard, not a substitute for the stronger checks Decision 6
# also names.
#
# Usage: cmake -D "MODULE_FILES=<semicolon-separated absolute paths>" -P CoreBoundaryCheck.cmake

if(NOT DEFINED MODULE_FILES)
    message(FATAL_ERROR "CoreBoundaryCheck.cmake: MODULE_FILES must be set with -D")
endif()

set(forbidden_pattern "^import[ \t]+mddlog\\.(sinks|adapter)\\.")
set(violations)

foreach(module_file IN LISTS MODULE_FILES)
    if(NOT EXISTS "${module_file}")
        message(FATAL_ERROR "CoreBoundaryCheck.cmake: '${module_file}' does not exist")
    endif()
    file(STRINGS "${module_file}" matches REGEX "${forbidden_pattern}")
    if(matches)
        list(APPEND violations "${module_file}: ${matches}")
    endif()
endforeach()

if(violations)
    string(REPLACE ";" "\n  " violations_text "${violations}")
    message(FATAL_ERROR
        "mddlog-core module(s) import a sink or adapter module - the governed core must not "
        "depend on either zone (ADR-001 Decision 6):\n  ${violations_text}")
endif()

message(STATUS "CoreBoundaryCheck: OK - no mddlog-core module imports a sink/adapter module")
