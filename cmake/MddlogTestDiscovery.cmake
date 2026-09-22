# MddlogTestDiscovery.cmake
#
# mddlog_discover_tests(<target>) registers one CTest entry per scenario a SpecLab test binary
# reports, so `ctest -R <name>` selects an individual scenario instead of only the whole
# executable. After the test binary links, it is run once with --list-tests, and an include file
# of add_test() calls and CTest label properties is generated from the name/label records it
# prints - the same POST_BUILD-discovery pattern Catch2's and GoogleTest's CMake integration
# modules use.
#
# Requires the target to link speclab::speclab: SpecLab's runMain() (speclab.runners.discovery)
# implements the --list-tests / --run=<name> contract this depends on.

set(_MDDLOG_TEST_DISCOVERY_IMPL_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/MddlogTestDiscoveryImpl.cmake")

function(mddlog_discover_tests TARGET)
    set(ctest_file_base "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_discovered")
    set(ctest_include_file "${ctest_file_base}_include.cmake")
    set(ctest_tests_file "${ctest_file_base}_tests.cmake")

    add_custom_command(
        TARGET ${TARGET} POST_BUILD
        BYPRODUCTS "${ctest_tests_file}"
        COMMAND "${CMAKE_COMMAND}"
                -D "TEST_TARGET=${TARGET}"
                -D "TEST_EXECUTABLE=$<TARGET_FILE:${TARGET}>"
                -D "TEST_OUTPUT_FILE=${ctest_tests_file}"
                -P "${_MDDLOG_TEST_DISCOVERY_IMPL_SCRIPT}"
        VERBATIM
    )

    file(WRITE "${ctest_include_file}"
        "if(EXISTS \"${ctest_tests_file}\")\n"
        "  include(\"${ctest_tests_file}\")\n"
        "else()\n"
        "  add_test(${TARGET}_NOT_BUILT ${TARGET}_NOT_BUILT)\n"
        "endif()\n"
    )

    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${ctest_include_file}")
endfunction()
