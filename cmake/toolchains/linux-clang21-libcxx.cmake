# Linux Clang toolchain, mirroring MduX's cmake/toolchains/linux-clang21-libcxx.cmake.
#
# CMake needs to be pointed at libc++'s own `libc++.modules.json`, and the compile must actually
# select libc++, for `import std` to resolve with upstream Clang. apt.llvm.org's libc++-21-dev
# ships that manifest, so this toolchain runs on every push (see .github/workflows/clang-build.yml).
if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The linux-clang21-libcxx toolchain is only valid on Linux hosts.")
endif()

set(_mddlog_llvm_root "$ENV{MDDLOG_LLVM_ROOT}")
if(NOT _mddlog_llvm_root)
    # apt.llvm.org installs versioned roots here; this is the layout clang-build.yml provisions.
    set(_mddlog_llvm_root "/usr/lib/llvm-21")
endif()
if(NOT IS_DIRECTORY "${_mddlog_llvm_root}")
    message(FATAL_ERROR
        "No LLVM 21 root at '${_mddlog_llvm_root}'. Set MDDLOG_LLVM_ROOT, or install the packages "
        "clang-build.yml installs: clang-21, libc++-21-dev, libc++abi-21-dev.")
endif()

set(CMAKE_C_COMPILER "${_mddlog_llvm_root}/bin/clang" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_mddlog_llvm_root}/bin/clang++" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${_mddlog_llvm_root}/bin/llvm-ar" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_mddlog_llvm_root}/bin/llvm-ranlib" CACHE FILEPATH "" FORCE)

# libc++ rather than libstdc++: the `std` module is shipped by the standard library, and only
# libc++ ships one this project builds against on Linux/Clang.
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")

# Where Debian/Ubuntu put `libc++.modules.json` is not fixed across LLVM packagings, so search the
# layouts that exist rather than asserting one. A wrong guess here would fail later and less
# legibly, as a missing `std` module rather than a missing file.
#
# The manifest is cached, and the compilers above are re-FORCED on every configure, so a bare
# `if(NOT DEFINED ...)` guard would be wrong in a way that is hard to see: re-configuring an
# existing build tree with a different MDDLOG_LLVM_ROOT would take the new root's clang++ and keep
# the old root's libc++ metadata. So record which root the cached manifest was discovered under,
# and re-discover when it moves. An operator's explicit -DCMAKE_CXX_STDLIB_MODULES_JSON has no
# recorded root and is left alone.
if(DEFINED CMAKE_CXX_STDLIB_MODULES_JSON AND NOT DEFINED MDDLOG_STDLIB_MODULES_JSON_ROOT)
    # Pinned by the operator; this file does not second-guess it.
elseif(NOT DEFINED CMAKE_CXX_STDLIB_MODULES_JSON
       OR NOT MDDLOG_STDLIB_MODULES_JSON_ROOT STREQUAL "${_mddlog_llvm_root}")
    file(GLOB_RECURSE _mddlog_libcxx_modules_json
        "${_mddlog_llvm_root}/lib/*/libc++.modules.json"
        "${_mddlog_llvm_root}/lib/libc++.modules.json"
        "${_mddlog_llvm_root}/share/libc++/*/libc++.modules.json")
    list(LENGTH _mddlog_libcxx_modules_json _mddlog_libcxx_modules_json_count)
    if(_mddlog_libcxx_modules_json_count EQUAL 0)
        message(FATAL_ERROR
            "No libc++.modules.json under '${_mddlog_llvm_root}'. This LLVM packaging does not "
            "ship the std module manifest, so `import std` cannot resolve for libc++ here.")
    endif()

    # `file(GLOB_RECURSE)` does not guarantee an order, so taking element 0 of the raw result
    # would let the chosen manifest depend on directory iteration order. Sort first, so the same
    # installation always selects the same manifest.
    list(SORT _mddlog_libcxx_modules_json)
    list(GET _mddlog_libcxx_modules_json 0 _mddlog_libcxx_modules_json_first)
    if(_mddlog_libcxx_modules_json_count GREATER 1)
        message(WARNING
            "Multiple libc++.modules.json under '${_mddlog_llvm_root}': "
            "${_mddlog_libcxx_modules_json}. Using '${_mddlog_libcxx_modules_json_first}'. Set "
            "CMAKE_CXX_STDLIB_MODULES_JSON explicitly to choose a different one.")
    endif()
    set(CMAKE_CXX_STDLIB_MODULES_JSON "${_mddlog_libcxx_modules_json_first}"
        CACHE FILEPATH "" FORCE)
    set(MDDLOG_STDLIB_MODULES_JSON_ROOT "${_mddlog_llvm_root}"
        CACHE INTERNAL "LLVM root the cached libc++ module manifest was discovered under")
endif()

foreach(_mddlog_required_tool
        "${CMAKE_CXX_COMPILER}"
        "${CMAKE_AR}"
        "${CMAKE_RANLIB}"
        "${CMAKE_CXX_STDLIB_MODULES_JSON}")
    if(NOT EXISTS "${_mddlog_required_tool}")
        message(FATAL_ERROR "Required Linux Clang toolchain input is missing: ${_mddlog_required_tool}")
    endif()
endforeach()
