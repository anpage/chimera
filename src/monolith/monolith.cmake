# Copyright (c) 2018 Kavawuvi
#
# This code is licensed under the GNU GPL v3.0 or any later version
# See LICENSE for more information.

# Define the source files
add_library(monolith MODULE
    src/monolith/monolith.c
    src/chimera/version.rc
)

# Set the name
set_target_properties(monolith PROPERTIES PREFIX "")
set_target_properties(monolith PROPERTIES OUTPUT_NAME "strings")

# lol
set_target_properties(monolith PROPERTIES LINK_FLAGS "-m32 -static-libgcc -static-libstdc++ -static -lwinpthread")
target_link_libraries(monolith chimera shlwapi ${CMAKE_CURRENT_SOURCE_DIR}/lib/libinvader.a ${CMAKE_CURRENT_SOURCE_DIR}/lib/libzstd.a)
