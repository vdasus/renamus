cmake_minimum_required(VERSION 3.10)
project(renamus CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# Path to a far2l source tree (the directory that contains far2l/, WinPort/).
# Only headers are used; the plugin resolves host symbols at load time.
if(NOT FAR2L_SRC)
    message(FATAL_ERROR
        "Set FAR2L_SRC to your far2l source tree, e.g.\n"
        "  cmake -DFAR2L_SRC=/path/to/far2l ..")
endif()
if(NOT EXISTS "${FAR2L_SRC}/far2l/far2sdk/farplug-wide.h")
    message(FATAL_ERROR "farplug-wide.h not found under ${FAR2L_SRC}/far2l/far2sdk")
endif()

add_library(renamus MODULE src/Renamus.cpp)

target_compile_definitions(renamus PRIVATE UNICODE WINPORT_DIRECT FAR_DONT_USE_INTERNALS)

target_include_directories(renamus PRIVATE
    src
    ${FAR2L_SRC}/far2l/far2sdk
    ${FAR2L_SRC}/WinPort)

set_target_properties(renamus PROPERTIES
    PREFIX ""
    SUFFIX ".far-plug-wide")

# WINPORT(MoveFile)/GetLastError live in the far2l host process; allow them to be
# left undefined in the module and resolved at load time.
if(APPLE)
    target_link_options(renamus PRIVATE -undefined dynamic_lookup -Wl,-no_fixup_chains)
endif()

# Lay out the installable plugin folder next to the binary:
#   <build>/renamus/plug/renamus.far-plug-wide
#   <build>/renamus/plug/RenamusEng.hlf
set(PLUGIN_DIR ${CMAKE_BINARY_DIR}/renamus/plug)
set_target_properties(renamus PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${PLUGIN_DIR})
add_custom_command(TARGET renamus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/configs/plug/RenamusEng.hlf ${PLUGIN_DIR}/RenamusEng.hlf)
