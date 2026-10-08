# CompilerSettings.cmake
# Handle compiler-specific configuration and generator optimizations

# Configure Ninja generator for optimal builds
if(CMAKE_GENERATOR STREQUAL "Ninja")
  # Enable colored output for Ninja
  set(CMAKE_COLOR_MAKEFILE ON)
  set(CMAKE_COLOR_DIAGNOSTICS ON)
  # Use response files for long command lines to handle very long command lines
  set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON)
  message(STATUS "Using Ninja build system with optimizations")
endif()

# Settings used to compile mddlog's own targets. They are PRIVATE: a consumer chooses its own
# warning level, exception model and diagnostics, and never inherits this project's (#121). CMake
# still records them in IMPORTED_CXX_MODULES_COMPILE_* so an installed consumer rebuilds mddlog's
# BMIs with the definitions the modules were compiled with.
function(configure_compiler_settings target_name)
  target_compile_options(${target_name} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/EHsc /permissive->
    $<$<CXX_COMPILER_ID:GNU>:-fconcepts-diagnostics-depth=2>
  )
endfunction()

# Version and compliance definitions read by the module interfaces (mddlog::getVersion(),
# mddlog::isMedicalComplianceEnabled()). PRIVATE since #121: they are not a macro API of a consumer
# translation unit; consumers query the exported functions instead.
function(configure_medical_compliance target_name version_major version_minor version_patch)
  target_compile_definitions(${target_name} PRIVATE
    # project(mddlog VERSION ...) is the single definition of the version; getVersion() reads it
    # from here rather than repeating the literal (docs/release-process.md, step 3).
    MDDLOG_VERSION_STRING="${version_major}.${version_minor}.${version_patch}"
    MDDLOG_MEDICAL_DEVICE_COMPLIANCE=1
  )
endfunction()
