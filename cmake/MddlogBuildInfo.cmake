# MddlogBuildInfo.cmake
#
# Writes mddlog-build-info.json, installed next to mddlogConfig.cmake: the exact source revision,
# toolchain, options, components and dependencies a package was built with (#121, ADR-007). It is a
# manifest for the integrator's configuration records, not a qualification: the qualified matrix is
# docs/compatibility-matrix.md, and a package built outside it says so only through these values.

# Source revision: Git when building a checkout, else the SOURCE_REVISION file that
# scripts/package-source.py writes into a source archive, else "unknown".
function(_mddlog_source_revision out_revision out_state)
    set(revision "unknown")
    set(state "unknown")
    find_package(Git QUIET)
    if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" rev-parse HEAD
            RESULT_VARIABLE result OUTPUT_VARIABLE head ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(result EQUAL 0 AND head MATCHES "^[0-9a-f]+$")
            set(revision "${head}")
            execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" status --porcelain
                --untracked-files=no
                RESULT_VARIABLE result OUTPUT_VARIABLE changes ERROR_QUIET)
            if(result EQUAL 0 AND changes STREQUAL "")
                set(state "clean-checkout")
            elseif(result EQUAL 0)
                set(state "modified-checkout")
            endif()
        endif()
    elseif(EXISTS "${PROJECT_SOURCE_DIR}/SOURCE_REVISION")
        # Line 1: commit; line 2: "commit" (release form) or "worktree" (uncommitted test archive).
        file(STRINGS "${PROJECT_SOURCE_DIR}/SOURCE_REVISION" lines LIMIT_COUNT 2)
        list(LENGTH lines line_count)
        if(line_count EQUAL 2)
            list(GET lines 0 commit)
            list(GET lines 1 kind)
            if(commit MATCHES "^[0-9a-f]+$" AND kind MATCHES "^(commit|worktree)$")
                set(revision "${commit}")
                set(state "source-archive-${kind}")
            endif()
        endif()
    endif()
    set(${out_revision} "${revision}" PARENT_SCOPE)
    set(${out_state} "${state}" PARENT_SCOPE)
endfunction()

function(_mddlog_json_bool out value)
    if(value)
        set(${out} true PARENT_SCOPE)
    else()
        set(${out} false PARENT_SCOPE)
    endif()
endfunction()

function(mddlog_write_build_info output)
    _mddlog_source_revision(revision revision_state)
    set(json "{}")
    string(JSON json SET "${json}" formatVersion 1)
    string(JSON json SET "${json}" package "\"mddlog\"")
    string(JSON json SET "${json}" version "\"${PROJECT_VERSION}\"")
    string(JSON json SET "${json}" sourceRevision "{}")
    string(JSON json SET "${json}" sourceRevision commit "\"${revision}\"")
    string(JSON json SET "${json}" sourceRevision state "\"${revision_state}\"")

    string(JSON json SET "${json}" toolchain "{}")
    foreach(field IN ITEMS
            "system|${CMAKE_SYSTEM_NAME}" "processor|${CMAKE_SYSTEM_PROCESSOR}"
            "compilerId|${CMAKE_CXX_COMPILER_ID}" "compilerVersion|${CMAKE_CXX_COMPILER_VERSION}"
            "cmakeVersion|${CMAKE_VERSION}" "generator|${CMAKE_GENERATOR}"
            "cxxFlags|${CMAKE_CXX_FLAGS}" "stdlibModulesJson|${CMAKE_CXX_STDLIB_MODULES_JSON}")
        string(FIND "${field}" "|" separator)
        string(SUBSTRING "${field}" 0 ${separator} key)
        math(EXPR value_start "${separator} + 1")
        string(SUBSTRING "${field}" ${value_start} -1 value)
        # Escape backslashes and quotes so Windows paths and flags stay valid JSON strings.
        string(REPLACE "\\" "\\\\" value "${value}")
        string(REPLACE "\"" "\\\"" value "${value}")
        string(JSON json SET "${json}" toolchain ${key} "\"${value}\"")
    endforeach()
    string(JSON json SET "${json}" toolchain configuration "\"@MDDLOG_CONFIG@\"")

    string(JSON json SET "${json}" options "{}")
    foreach(option IN ITEMS MDDLOG_BUILD_FILE_STORAGE MDDLOG_BUILD_AUDIT_TOOLS
            ENABLE_SANITIZER_ADDRESS ENABLE_SANITIZER_LEAK ENABLE_SANITIZER_UNDEFINED_BEHAVIOR
            ENABLE_SANITIZER_THREAD ENABLE_SANITIZER_MEMORY ENABLE_COVERAGE ENABLE_IPO)
        _mddlog_json_bool(value "${${option}}")
        string(JSON json SET "${json}" options ${option} ${value})
    endforeach()

    set(components "[\"core\",\"full\"]")
    if(MDDLOG_BUILD_FILE_STORAGE)
        string(JSON components SET "${components}" 2 "\"file_storage\"")
    endif()
    if(MDDLOG_BUILD_AUDIT_TOOLS)
        string(JSON count LENGTH "${components}")
        string(JSON components SET "${components}" ${count} "\"audit_tool\"")
    endif()
    string(JSON json SET "${json}" components "${components}")

    # Link-time dependencies a consumer inherits. Test-only SpecLab and build tools are not part of
    # the installed package; docs/compatibility-matrix.md pins them for the qualified profiles.
    string(JSON json SET "${json}" dependencies
        "[{\"name\":\"C++ standard library and its std module\",\"scope\":\"deployed\",\"provider\":\"toolchain\"},{\"name\":\"Threads::Threads\",\"scope\":\"deployed\",\"provider\":\"toolchain\"}]")

    string(REPLACE "@MDDLOG_CONFIG@" "$<CONFIG>" json "${json}")
    file(GENERATE OUTPUT "${output}" CONTENT "${json}\n")
endfunction()
