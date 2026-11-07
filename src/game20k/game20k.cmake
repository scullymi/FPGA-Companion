# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 scullymi
# game20k's own code as a library of the kind the Pico SDK uses: an INTERFACE library, so the
# sources build with the settings of the target that links it, FreeRTOS and lwIP among them.
# Included by ../rp2040/CMakeLists.txt with GAME20K on, before pico_sdk_init(). The define
# GAME20K reaches every source of the firmware, the files of FPGA Companion test it.

if(NOT BOARD STREQUAL PICO2)
  message(FATAL_ERROR "GAME20K builds for BOARD=PICO2 only, the Pico 2 W of game20k")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/game20k_sources.cmake)

# pico_mbedtls reads this when pico_sdk_init() creates its target
set(PICO_MBEDTLS_CONFIG_FILE ${GAME20K_DIR}/game20k_mbedtls_config.h)

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
