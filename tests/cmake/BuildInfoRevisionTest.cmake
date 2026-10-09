# The build manifest names the revision present at build time, not at configuration time (#121):
# a commit or an edit followed by `cmake --build` without reconfiguring must update it.
foreach(required_variable SOURCE_DIR BINARY_DIR GENERATOR GIT_EXECUTABLE)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "BuildInfoRevisionTest.cmake: ${required_variable} must be set")
    endif()
endforeach()

set(fixture "${BINARY_DIR}/build-info-revision")
set(project_dir "${fixture}/project")
set(build_dir "${fixture}/build")
file(REMOVE_RECURSE "${fixture}")
file(MAKE_DIRECTORY "${project_dir}")
file(WRITE "${project_dir}/CMakeLists.txt" "\
cmake_minimum_required(VERSION 4.0)
project(buildinforevision VERSION 1.2.3 LANGUAGES NONE)
include(\"${SOURCE_DIR}/cmake/MddlogBuildInfo.cmake\")
mddlog_write_build_info(\"\${CMAKE_BINARY_DIR}/build-info-\$<CONFIG>.json\")
")
file(WRITE "${project_dir}/source.txt" "first\n")

function(run description)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${description} failed (${result}):\n${output}\n${error}")
    endif()
endfunction()

set(git "${GIT_EXECUTABLE}" -C "${project_dir}" -c user.name=test -c user.email=test@example.invalid
    -c commit.gpgsign=false)
function(commit message)
    run("git add" ${git} add -A)
    run("git commit" ${git} commit -q -m "${message}")
endfunction()

function(expect_revision when expected_state)
    run("cmake --build (${when})" "${CMAKE_COMMAND}" --build "${build_dir}")
    execute_process(COMMAND ${git} rev-parse HEAD OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE)
    file(READ "${build_dir}/build-info-Release.json" manifest)
    string(JSON commit GET "${manifest}" sourceRevision commit)
    string(JSON state GET "${manifest}" sourceRevision state)
    if(NOT commit STREQUAL head OR NOT state STREQUAL expected_state)
        message(FATAL_ERROR "${when}: manifest names ${commit} (${state}), expected ${head} (${expected_state})")
    endif()
endfunction()

run("git init" ${git} init -q)
commit("first")
run("configure" "${CMAKE_COMMAND}" -S "${project_dir}" -B "${build_dir}" -G "${GENERATOR}"
    -D CMAKE_BUILD_TYPE=Release)
expect_revision("first build" "clean-checkout")

# No reconfiguration from here on: only the build may refresh the manifest.
file(WRITE "${project_dir}/source.txt" "second\n")
commit("second")
expect_revision("build after a commit" "clean-checkout")

file(WRITE "${project_dir}/source.txt" "edited\n")
expect_revision("build after an edit" "modified-checkout")

message(STATUS "build-info revision follows commits and edits without reconfiguration")
