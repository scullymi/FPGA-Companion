# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 scullymi
# game20k's own code in the BL616 firmware, the Companion of the Tang Nano 20K on the board.
# Included by ../bl616/CMakeLists.txt with GAME20K on, after the target app exists. The
# definitions reach every source of the firmware, the files of FPGA Companion test GAME20K.
# RetroAchievements stay off: the BL616 build has no TLS client of its own (yet).

include(${CMAKE_CURRENT_LIST_DIR}/game20k_sources.cmake)
if(GAME20K_RA)
  message(FATAL_ERROR "GAME20K_RA is not built for the BL616, set -DGAME20K_RA=OFF")
endif()

target_sources(app PRIVATE ${GAME20K_SOURCES} ${GAME20K_GAMES_TABLE} ${GAME20K_MENU_TABLE}
  ${RCHEEVOS_SOURCES})
sdk_add_include_directories(${RCHEEVOS_DIR}/include ${RCHEEVOS_DIR}/src)
sdk_add_compile_definitions(-DGAME20K -DGAME20K_RA=0 -DRC_NO_THREADS=1
  -DGAME20K_VERSION="${GAME20K_VERSION}" -DGAME20K_PLATFORM="${GAME20K_PLATFORM}")
