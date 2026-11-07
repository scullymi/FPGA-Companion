# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 scullymi
# game20k's own sources and their settings, the same for every MCU. game20k.cmake (Pico) and
# game20k_bl616.cmake (BL616) take them into their firmware.

set(GAME20K_DIR ${CMAKE_CURRENT_LIST_DIR})
set(RCHEEVOS_DIR ${GAME20K_DIR}/../rcheevos)

# how this firmware introduces itself to RetroAchievements (User-Agent). The version here is the
# fallback for a build without g20k.py, 0.0.0 so that such a build never looks like a release
# to the server.
if(NOT DEFINED GAME20K_VERSION)
  set(GAME20K_VERSION "0.0.0")
endif()
if(NOT DEFINED GAME20K_PLATFORM)
  set(GAME20K_PLATFORM "Tang Nano 20K")
endif()

# the game table and the menus of the cores, C files g20k.py firmware generates from the ROM
# manifests and the menu sources. A build without them takes the examples, which hold neither.
if(NOT DEFINED GAME20K_GAMES_TABLE)
  set(GAME20K_GAMES_TABLE ${GAME20K_DIR}/ra_games_example.c)
endif()
if(NOT DEFINED GAME20K_MENU_TABLE)
  set(GAME20K_MENU_TABLE ${GAME20K_DIR}/menus_example.c)
endif()
message(STATUS "game20k: games table ${GAME20K_GAMES_TABLE}")
message(STATUS "game20k: menu table ${GAME20K_MENU_TABLE}")
# they include ra_games.h and menus.h
set_source_files_properties(${GAME20K_GAMES_TABLE} ${GAME20K_MENU_TABLE}
  PROPERTIES INCLUDE_DIRECTORIES ${GAME20K_DIR})

option(GAME20K_RA "RetroAchievements, without it no network client and no achievements" ON)

set(GAME20K_SOURCES
  banner.c core_boot.c game.c game_switch.c games.c games_file.c menu_pages.c menus.c
  ra_games.c ram_mirror.c)
if(GAME20K_RA)
  list(APPEND GAME20K_SOURCES ra_events.c ra_mac.c ra_net.c ra_patch.c ra_queue.c ra_slim.c
    ra_state.c ra_task.c ra_text.c)
endif()
list(TRANSFORM GAME20K_SOURCES PREPEND ${GAME20K_DIR}/)
# a warning in our own code stops the build, also the one the target of the board lets pass
set_source_files_properties(${GAME20K_SOURCES} PROPERTIES COMPILE_OPTIONS
  "-Werror;-Werror=incompatible-pointer-types")

# rcheevos: the runtime that evaluates the conditions and the reader for the set (api), no
# rc_client. rc_validate.c stays out, only the editor needs it. The table of the games takes its
# md5 in any case.
set(RCHEEVOS_SOURCES util/md5.c)
if(GAME20K_RA)
  list(APPEND RCHEEVOS_SOURCES
    runtime/rc_alloc.c runtime/rc_condition.c runtime/rc_condset.c runtime/rc_consoleinfo.c
    runtime/rc_format.c runtime/rc_lboard.c runtime/rc_memref.c runtime/rc_operand.c
    runtime/rc_richpresence.c runtime/rc_runtime.c runtime/rc_runtime_progress.c
    runtime/rc_trigger.c runtime/rc_typed_value.c runtime/rc_value.c
    api/rc_api_common.c api/rc_api_runtime.c api/rc_api_user.c
    util/rc_util.c util/rc_compat.c util/rc_version.c)
endif()
list(TRANSFORM RCHEEVOS_SOURCES PREPEND ${RCHEEVOS_DIR}/src/)
