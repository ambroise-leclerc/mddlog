option(ENABLE_USER_LINKER "Enable a specific linker if available" OFF)

include(CheckCXXCompilerFlag)

set(USER_LINKER_OPTION
  "lld"
  CACHE STRING "Linker to be used")
set(USER_LINKER_OPTION_VALUES "lld" "gold" "bfd")
set_property(CACHE USER_LINKER_OPTION PROPERTY STRINGS ${USER_LINKER_OPTION_VALUES})
list(
  FIND
  USER_LINKER_OPTION_VALUES
  ${USER_LINKER_OPTION}
  USER_LINKER_OPTION_INDEX)

if(${USER_LINKER_OPTION_INDEX} EQUAL -1)
  message(
    STATUS
      "Using custom linker: '${USER_LINKER_OPTION}', explicitly supported entries are ${USER_LINKER_OPTION_VALUES}")
endif()

# A linker choice concerns the executables this build links (tests, examples, tools), not a
# consumer of the installed package: apply it to this directory's links, never as an exported
# usage requirement (#121). It used to be added as a compile option, where it selected nothing.
function(configure_linker)
  if(NOT ENABLE_USER_LINKER)
    return()
  endif()

  set(LINKER_FLAG "-fuse-ld=${USER_LINKER_OPTION}")

  check_cxx_compiler_flag(${LINKER_FLAG} CXX_SUPPORTS_USER_LINKER)
  if(CXX_SUPPORTS_USER_LINKER)
    add_link_options(${LINKER_FLAG})
  endif()
endfunction()
