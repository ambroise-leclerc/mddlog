# Independent consumer of mddlog (#121). Deliberately a separate CMake project, outside mddlog's own
# build graph: it sees mddlog only as an integrator would, either through the installed package
# (find_package) or through a source tree it adds itself (add_subdirectory of a checkout or of an
# extracted source archive). cmake/TestInstallConsumer.cmake drives it from CTest; it can also be
# configured by hand, which is how docs/consumer-requirements.md presents it.
#
#   MDDLOG_CONSUMER_MODE     installed | source
#   MDDLOG_CONSUMER_PROFILE  core (mddlog::core only) | full (core, umbrella, optional components)
#   MDDLOG_SOURCE_DIR        the mddlog source tree, for MDDLOG_CONSUMER_MODE=source
#   MDDLOG_CONSUMER_EXPECTED_VERSION  version the full programs must observe
#   MDDLOG_CONSUMER_FILE_STORAGE / MDDLOG_CONSUMER_AUDIT_TOOL  also require those components
cmake_minimum_required(VERSION 4.0.0)

# The consumer, not the package, selects CMake's import std gate: it must precede project(), so a
# package found later could not set it effectively.
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.4")
    message(FATAL_ERROR "This consumer has not qualified CMake ${CMAKE_VERSION}'s import std gate.")
elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
else()
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
endif()

# Also before project(): CMake creates the `std` module target while enabling CXX, from the
# standard and extensions selected at that point. Clang refuses to load a std BMI built as gnu++23
# into the -std=c++23 compilations of mddlog's modules ("GNU extensions was enabled").
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(MddlogPackageConsumer LANGUAGES CXX)

# This consumer's own sources `import std`; mddlog does not switch that on for it.
set(CMAKE_CXX_MODULE_STD ON)

set(MDDLOG_CONSUMER_MODE "installed" CACHE STRING "installed or source")
set(MDDLOG_CONSUMER_PROFILE "full" CACHE STRING "core or full")
set(MDDLOG_CONSUMER_EXPECTED_VERSION "" CACHE STRING "Version the full programs must observe")
option(MDDLOG_CONSUMER_FILE_STORAGE "Require and exercise the file_storage component" OFF)
option(MDDLOG_CONSUMER_AUDIT_TOOL "Require and exercise the audit_tool component" OFF)
if(NOT MDDLOG_CONSUMER_MODE MATCHES "^(installed|source)$" OR NOT MDDLOG_CONSUMER_PROFILE MATCHES "^(core|full)$")
    message(FATAL_ERROR "MDDLOG_CONSUMER_MODE must be installed|source and MDDLOG_CONSUMER_PROFILE core|full.")
endif()
if(MDDLOG_CONSUMER_PROFILE STREQUAL "core" AND (MDDLOG_CONSUMER_FILE_STORAGE OR MDDLOG_CONSUMER_AUDIT_TOOL))
    message(FATAL_ERROR "The core profile links mddlog::core only; optional components need the full profile.")
endif()

set(consumer_components core)
if(MDDLOG_CONSUMER_PROFILE STREQUAL "full")
    list(APPEND consumer_components full)
endif()
if(MDDLOG_CONSUMER_FILE_STORAGE)
    list(APPEND consumer_components file_storage)
endif()
if(MDDLOG_CONSUMER_AUDIT_TOOL)
    list(APPEND consumer_components audit_tool)
endif()

if(MDDLOG_CONSUMER_MODE STREQUAL "installed")
    find_package(mddlog CONFIG REQUIRED COMPONENTS ${consumer_components})
    set(consumer_audit_tool mddlog::audit_tool)
else()
    if(NOT IS_DIRECTORY "${MDDLOG_SOURCE_DIR}")
        message(FATAL_ERROR "MDDLOG_CONSUMER_MODE=source needs MDDLOG_SOURCE_DIR.")
    endif()
    # The consumer decides which parts of mddlog it builds; tests and examples stay off.
    set(MDDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MDDLOG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(MDDLOG_BUILD_FILE_STORAGE ${MDDLOG_CONSUMER_FILE_STORAGE} CACHE BOOL "" FORCE)
    set(MDDLOG_BUILD_AUDIT_TOOLS ${MDDLOG_CONSUMER_AUDIT_TOOL} CACHE BOOL "" FORCE)
    add_subdirectory("${MDDLOG_SOURCE_DIR}" mddlog)
    foreach(required_target IN ITEMS mddlog::core mddlog::mddlog)
        if(NOT TARGET ${required_target})
            message(FATAL_ERROR "The mddlog source tree does not provide ${required_target}.")
        endif()
    endforeach()
    set(consumer_audit_tool mddlog_audit)
endif()

# The consumer's own warning policy, applied to its own sources only. mddlog's warnings must not
# reach these targets, and mddlog's interface must compile cleanly under this policy.
set(consumer_warnings)
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(consumer_warnings /W4 /WX)
else()
    set(consumer_warnings -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror)
    # Known consumer constraint (docs/consumer-requirements.md): -Wextra on GCC 16 and Clang 21
    # reports -Wmissing-field-initializers for designated initializers that omit defaulted members,
    # which is how mddlog's option structs are meant to be written ({.time = ..., .message = ...}).
    list(APPEND consumer_warnings -Wno-missing-field-initializers)
endif()

set(consumer_root "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(consumer_programs)
function(add_consumer_program name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${ARGN})
    target_compile_options(${name} PRIVATE ${consumer_warnings})
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${name} PRIVATE /EHsc)
    endif()
    set(consumer_programs ${consumer_programs} ${name} PARENT_SCOPE)
endfunction()

# A translation unit that directly imports a governed module links the target providing it:
# CMake 4.1 with GCC does not place transitively linked modules in its module mapper.
add_consumer_program(consumer_core "${consumer_root}/CoreConsumer.cpp" mddlog::core)
add_consumer_program(consumer_inventory "${consumer_root}/../../examples/ContextualInventory.cpp" mddlog::core)
if(MDDLOG_CONSUMER_PROFILE STREQUAL "full")
    # Imports only the umbrella and the Log facade and links only mddlog::mddlog, so the full
    # target must bring mddlog::core, its modules and its library through its declared dependency.
    add_consumer_program(consumer_full FullConsumer.cpp mddlog::mddlog)
    target_compile_definitions(consumer_full PRIVATE
        MDDLOG_CONSUMER_EXPECTED_VERSION="${MDDLOG_CONSUMER_EXPECTED_VERSION}")
    add_consumer_program(consumer_context "${consumer_root}/ContextAdapterConsumer.cpp" mddlog::mddlog)
    add_consumer_program(consumer_migration MigrationExamples.cpp mddlog::mddlog)
endif()
if(MDDLOG_CONSUMER_FILE_STORAGE)
    add_consumer_program(consumer_file_storage "${consumer_root}/FileStorageConsumer.cpp" mddlog::mddlog)
endif()

# Each program's real path per configuration, so no generator-specific layout is guessed.
set(consumer_paths "")
foreach(program IN LISTS consumer_programs)
    string(APPEND consumer_paths "${program}=$<TARGET_FILE:${program}>\n")
endforeach()
if(MDDLOG_CONSUMER_AUDIT_TOOL)
    string(APPEND consumer_paths "audit_tool=$<TARGET_FILE:${consumer_audit_tool}>\n")
endif()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/mddlog-consumer-$<CONFIG>.txt" CONTENT "${consumer_paths}")
