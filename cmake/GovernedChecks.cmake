# Build-time evidence for ADR-001 Decision 6. Keep these profiles independent.
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

set(governed_manifest "${CMAKE_CURRENT_BINARY_DIR}/governed-$<CONFIG>.txt")
set(governed_content "sources=$<JOIN:$<TARGET_PROPERTY:mddlog-core,CXX_MODULE_SET_cxx_modules>,;>\n")
string(APPEND governed_content "extra_sources=$<JOIN:$<TARGET_PROPERTY:mddlog-core,SOURCES>,;>\n")
string(APPEND governed_content "objects=$<JOIN:$<TARGET_OBJECTS:mddlog-core>,;>\n")
foreach(target IN ITEMS mddlog-core mddlog_options mddlog_warnings Threads::Threads)
    foreach(property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES
            INTERFACE_LINK_LIBRARIES_DIRECT INTERFACE_LINK_LIBRARIES_DIRECT_EXCLUDE
            LINK_OPTIONS INTERFACE_LINK_OPTIONS)
        string(APPEND governed_content
            "${target}|${property}=$<TARGET_GENEX_EVAL:${target},$<TARGET_PROPERTY:${target},${property}>>\n")
    endforeach()
endforeach()
file(GENERATE OUTPUT "${governed_manifest}" CONTENT "${governed_content}")

# Module interface objects alone can contain only initializers: exercise template bodies too.
add_library(mddlog_core_probe OBJECT ${PROJECT_SOURCE_DIR}/tests/boundary/CoreInstantiation.cpp)
target_link_libraries(mddlog_core_probe PRIVATE mddlog::core)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/governed-probe-$<CONFIG>.txt"
    CONTENT "$<JOIN:$<TARGET_OBJECTS:mddlog_core_probe>,\n>\n")

# Deliberate violations are compiled but never linked into the library or test runner.
foreach(profile IN ITEMS Allocation Exception)
    string(TOLOWER "${profile}" profile_name)
    add_library(governed_bad_${profile_name} OBJECT ${PROJECT_SOURCE_DIR}/tests/boundary/Forbidden${profile}.cpp)
    set_target_properties(governed_bad_${profile_name} PROPERTIES CXX_MODULE_STD OFF)
    target_compile_options(governed_bad_${profile_name} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/governed-bad-${profile_name}-$<CONFIG>.txt"
        CONTENT "$<JOIN:$<TARGET_OBJECTS:governed_bad_${profile_name}>,\n>\n")
endforeach()

if(MSVC)
    find_program(governed_symbol_tool NAMES dumpbin REQUIRED)
    set(governed_symbol_kind dumpbin)
else()
    get_filename_component(governed_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(governed_symbol_tool NAMES llvm-nm llvm-nm-21 nm
        HINTS "${governed_compiler_dir}" REQUIRED)
    set(governed_symbol_kind nm)
endif()

set(governed_checker "${PROJECT_SOURCE_DIR}/scripts/check-governed.py")
set(governed_arguments --root "${PROJECT_SOURCE_DIR}" --manifest "${governed_manifest}"
    --probe "${CMAKE_CURRENT_BINARY_DIR}/governed-probe-$<CONFIG>.txt"
    --tool "${governed_symbol_tool}" --kind "${governed_symbol_kind}")
foreach(check IN ITEMS graph source allocation exception)
    add_test(NAME build.core.${check}
        COMMAND "${Python3_EXECUTABLE}" "${governed_checker}" ${check} ${governed_arguments})
    set_tests_properties(build.core.${check} PROPERTIES LABELS "build;governed")
endforeach()
add_test(NAME build.core.negativeControls
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/boundary/TestGovernedChecks.py"
        ${governed_arguments}
        --bad-allocation "${CMAKE_CURRENT_BINARY_DIR}/governed-bad-allocation-$<CONFIG>.txt"
        --bad-exception "${CMAKE_CURRENT_BINARY_DIR}/governed-bad-exception-$<CONFIG>.txt")
set_tests_properties(build.core.negativeControls PROPERTIES LABELS "build;governed")
