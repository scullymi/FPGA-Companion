# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 scullymi
# game20k's own code as a library of the kind the Pico SDK uses: an INTERFACE library, so the
# sources build with the settings of the target that links it, FreeRTOS and lwIP among them.
# Included by ../rp2040/CMakeLists.txt with GAME20K on, before pico_sdk_init(). The define
# GAME20K reaches every source of the firmware, the files of FPGA Companion test it.

if(NOT BOARD STREQUAL PICO2)
  message(FATAL_ERROR "GAME20K builds for BOARD=PICO2 only, the Pico 2 W of game20k")
endif()

set(GAME20K_DIR ${CMAKE_CURRENT_LIST_DIR})
set(RCHEEVOS_DIR ${GAME20K_DIR}/../rcheevos)

# pico_mbedtls reads this when pico_sdk_init() creates its target
set(PICO_MBEDTLS_CONFIG_FILE ${GAME20K_DIR}/game20k_mbedtls_config.h)

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

add_library(game20k INTERFACE)
target_sources(game20k INTERFACE
  ${GAME20K_SOURCES} ${GAME20K_GAMES_TABLE} ${GAME20K_MENU_TABLE} ${RCHEEVOS_SOURCES})
target_include_directories(game20k INTERFACE ${RCHEEVOS_DIR}/include ${RCHEEVOS_DIR}/src)
# malloc returns NULL when the SDK heap is out instead of stopping the firmware. How much a set
# takes is up to the server, beyond our reach: what finds no room beside the parsed set comes
# once it is freed, else it stays off and hardcore is blocked (ra_patch.c), a request that finds
# too little fails and is asked again, and the game goes on. Our own mallocs check for NULL. A
# large set once stopped the firmware at every start.
target_compile_definitions(game20k INTERFACE
  GAME20K RC_NO_THREADS=1 PICO_MALLOC_PANIC=0
  GAME20K_VERSION="${GAME20K_VERSION}" GAME20K_PLATFORM="${GAME20K_PLATFORM}")
# mbedTLS: the digest of a ROM. HTTPS to RetroAchievements: the HTTP client, lwIP's TLS layer
target_link_libraries(game20k INTERFACE pico_mbedtls)
if(GAME20K_RA)
  target_compile_definitions(game20k INTERFACE GAME20K_RA=1)
  target_link_libraries(game20k INTERFACE pico_lwip_http pico_lwip_mbedtls)
else()
  target_compile_definitions(game20k INTERFACE GAME20K_RA=0)
endif()
