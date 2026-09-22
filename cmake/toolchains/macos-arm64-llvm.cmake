# Supported macOS toolchain, mirroring MduX's cmake/toolchains/macos-arm64-llvm.cmake.
#
# mddlog deliberately supports one reproducible macOS configuration: Apple Silicon, upstream
# LLVM/Clang 21.1.8, libc++, Ninja, and CMake 4.3.1. AppleClang and Homebrew GCC do not provide
# the same C++23 named-module surface.
if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "The macos-arm64-llvm toolchain is only valid on macOS hosts.")
endif()
if(NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    message(FATAL_ERROR
        "mddlog's verified macOS target is Apple Silicon (arm64); host architecture is "
        "${CMAKE_HOST_SYSTEM_PROCESSOR}.")
endif()
if(NOT CMAKE_VERSION VERSION_EQUAL "4.3.1")
    message(FATAL_ERROR
        "The verified macOS toolchain requires CMake 4.3.1 exactly; found ${CMAKE_VERSION}.")
endif()

set(_mddlog_llvm_root "$ENV{MDDLOG_LLVM_ROOT}")
if(NOT _mddlog_llvm_root)
    find_program(_mddlog_brew brew PATHS /opt/homebrew/bin NO_DEFAULT_PATH)
    if(_mddlog_brew)
        # `llvm@21` before `llvm`, because that is the formula CI installs and the one that stays
        # at this project's verified version. Unversioned `llvm` follows whatever Homebrew's
        # current LLVM is, so probing it first sends a contributor who installed exactly what CI
        # installs into the version rejection below.
        foreach(_mddlog_brew_formula llvm@21 llvm)
            execute_process(
                COMMAND "${_mddlog_brew}" --prefix ${_mddlog_brew_formula}
                RESULT_VARIABLE _mddlog_brew_result
                OUTPUT_VARIABLE _mddlog_brew_prefix
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            if(_mddlog_brew_result EQUAL 0 AND IS_DIRECTORY "${_mddlog_brew_prefix}")
                set(_mddlog_llvm_root "${_mddlog_brew_prefix}")
                break()
            endif()
        endforeach()
    endif()
endif()
if(NOT _mddlog_llvm_root)
    message(FATAL_ERROR
        "Set MDDLOG_LLVM_ROOT to an upstream LLVM 21.1.8 installation, or install the formula CI "
        "uses: brew install llvm@21.")
endif()

set(CMAKE_CXX_COMPILER "${_mddlog_llvm_root}/bin/clang++" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${_mddlog_llvm_root}/bin/llvm-ar" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_mddlog_llvm_root}/bin/llvm-ranlib" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_STDLIB_MODULES_JSON
    "${_mddlog_llvm_root}/lib/c++/libc++.modules.json" CACHE FILEPATH "" FORCE)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
execute_process(
    COMMAND xcrun --sdk macosx --show-sdk-path
    RESULT_VARIABLE _mddlog_sdk_result
    OUTPUT_VARIABLE _mddlog_macos_sdk
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _mddlog_sdk_result EQUAL 0 OR NOT IS_DIRECTORY "${_mddlog_macos_sdk}")
    message(FATAL_ERROR "xcrun could not locate the active macOS SDK.")
endif()
set(CMAKE_OSX_SYSROOT "${_mddlog_macos_sdk}" CACHE PATH "" FORCE)
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")

foreach(_mddlog_required_tool
        "${CMAKE_CXX_COMPILER}"
        "${CMAKE_AR}"
        "${CMAKE_RANLIB}"
        "${CMAKE_CXX_STDLIB_MODULES_JSON}")
    if(NOT EXISTS "${_mddlog_required_tool}")
        message(FATAL_ERROR "Required macOS toolchain input is missing: ${_mddlog_required_tool}")
    endif()
endforeach()
