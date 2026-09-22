# Examples CMakeLists.txt for MddLog

# Basic usage example
add_executable(mddlog_basic_usage basic_usage.cpp)
target_link_libraries(mddlog_basic_usage PRIVATE mddlog::mddlog)
target_compile_features(mddlog_basic_usage PRIVATE cxx_std_23)

# Simple usage example with static Log helper
add_executable(mddlog_simple_usage simple_usage.cpp)
target_link_libraries(mddlog_simple_usage PRIVATE mddlog::mddlog)
target_compile_features(mddlog_simple_usage PRIVATE cxx_std_23)

# Link to std module if available
if(TARGET __CMAKE::CXX23)
    target_link_libraries(mddlog_basic_usage PRIVATE __CMAKE::CXX23)
    target_link_libraries(mddlog_simple_usage PRIVATE __CMAKE::CXX23)
endif()

# Set compiler-specific options
if(MSVC)
    target_compile_options(mddlog_basic_usage PRIVATE /std:c++latest)
    target_compile_options(mddlog_simple_usage PRIVATE /std:c++latest)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(mddlog_basic_usage PRIVATE -std=c++23)
    target_compile_options(mddlog_simple_usage PRIVATE -std=c++23)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(mddlog_basic_usage PRIVATE -std=c++23)
    target_compile_options(mddlog_simple_usage PRIVATE -std=c++23)
endif()

# Add to examples folder in IDE
set_target_properties(mddlog_basic_usage PROPERTIES
    FOLDER "Examples"
    OUTPUT_NAME "basic_usage"
)

set_target_properties(mddlog_simple_usage PROPERTIES
    FOLDER "Examples"
    OUTPUT_NAME "simple_usage"
)

# Install examples (optional)
if(MDDLOG_INSTALL_EXAMPLES)
    install(TARGETS mddlog_basic_usage mddlog_simple_usage
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/examples
    )
endif()