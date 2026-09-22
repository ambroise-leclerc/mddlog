# MddlogTestDiscoveryImpl.cmake
#
# Run in script mode (-P) by the POST_BUILD command in MddlogTestDiscovery.cmake.
# Executes TEST_EXECUTABLE --list-tests, and writes one add_test() per line to
# TEST_OUTPUT_FILE. Each input line is name<TAB>comma-separated-labels. Each
# registered test invokes the same executable with --run=<name>, so a single
# build produces one binary and N ctest entries with matching CTest labels.

execute_process(
    COMMAND "${TEST_EXECUTABLE}" --list-tests
    OUTPUT_VARIABLE test_names_raw
    RESULT_VARIABLE list_result
)

if(NOT list_result EQUAL 0)
    message(FATAL_ERROR "mddlog_discover_tests: '${TEST_EXECUTABLE} --list-tests' failed with ${list_result}")
endif()

# Split on newlines, drop empty lines from trailing separators.
string(REPLACE "\n" ";" test_names "${test_names_raw}")

set(content "")
foreach(test_record IN LISTS test_names)
    string(FIND "${test_record}" "\t" label_separator)
    if(label_separator EQUAL -1)
        set(name "${test_record}")
        set(labels "")
    else()
        string(SUBSTRING "${test_record}" 0 ${label_separator} name)
        math(EXPR labels_start "${label_separator} + 1")
        string(SUBSTRING "${test_record}" ${labels_start} -1 labels)
    endif()

    string(STRIP "${name}" name)
    if(NOT name STREQUAL "")
        # Escape characters CTest test names / CMake strings care about.
        string(REPLACE "\\" "\\\\" escaped_name "${name}")
        string(REPLACE "\"" "\\\"" escaped_name "${escaped_name}")
        # Deliberately the old-style positional add_test(<name> <command> [args...]) rather than
        # add_test(NAME ... COMMAND ...) - see MduXTestDiscoveryImpl.cmake upstream for the CTest
        # version-specific mis-parsing this works around; mirrored here for the same reason.
        string(APPEND content
            "add_test(\"${TEST_TARGET}::${escaped_name}\" \"${TEST_EXECUTABLE}\" \"--run=${escaped_name}\")\n")
        if(NOT labels STREQUAL "")
            string(REPLACE "," ";" labels "${labels}")
            string(REPLACE "\\" "\\\\" escaped_labels "${labels}")
            string(REPLACE "\"" "\\\"" escaped_labels "${escaped_labels}")
            string(APPEND content
                "set_tests_properties(\"${TEST_TARGET}::${escaped_name}\" PROPERTIES LABELS \"${escaped_labels}\")\n")
        endif()
    endif()
endforeach()

file(WRITE "${TEST_OUTPUT_FILE}" "${content}")
