#
# CMakeLists.txt - FPGA Companion for rp2040
#
#
# to build for waveshare rp2040-zero do
#  cmake -DBOARD=WS2040ZERO ..
# to build for Pico2 or Pico2-W do
#  cmake -DBOARD=PICO2 ..
#
# You might need to delete CMakeCache.txt when changing the BOARD

cmake_minimum_required(VERSION 3.13)

set(PROJECT fpga_companion)

message(" __  __ _ ____ _____ _")
message(" |  \\/  (_) ___|_   _| | ___")
message(" | |\\/| | \\___ \\ | | | |/ o_)")
message(" |_|  |_|_|____/ |_| |_|\\___|")
message(" http://github.com/MiSTle-Dev")

message("==============================")
message("      Board is ${BOARD}")
message("==============================")

# workaround for VS Code extension for Raspberry Pi Pico development
set(PICO_BOARD pico_w CACHE STRING "Board type")

# The BOARD option selects which board to build for
# Regular Pico (W) by default
set(BOARD PICO CACHE STRING "Board type. Can be PICO, PICO2, WS2040ZERO, SH20KLITE, DEV20K, MSP20K, DEV25K")

set(BOARDS PICO PICO2 WS2040ZERO SH20KLITE DEV20K MSP20K DEV25K)
if( NOT BOARD IN_LIST BOARDS)
    message(FATAL_ERROR "BOARD must be one of ${BOARDS}")
endif()

# This will also work for the the Pico-W and the regular Pico. The
# firmware detects it and acts accordingly
set(PICO2BOARDS PICO2 DEV25K)
if( BOARD IN_LIST PICO2BOARDS)
   message("Building for RP2350")
   set(PICO_BOARD pico2_w)
   set(TARGET rp2350)
else()
   message("Building for RP2040")
   set(PICO_BOARD pico_w)
   set(TARGET rp2040)
endif()

# Pull in SDK (must be before project)
include(./pico_sdk_import.cmake)

project(${PROJECT} C CXX ASM)

# USB host on the Pico's own micro USB port (OTG adapter) instead of PIO USB on GP2/GP3
option(NATIVE_USB "Use the Pico's own micro USB port as USB host" OFF)
if(NATIVE_USB)
  message("Native USB host port selected")
  add_compile_definitions(CFG_TUH_RPI_PIO_USB=0)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if (PICO_SDK_VERSION_STRING VERSION_LESS "2.2.0")
    message(FATAL_ERROR "Raspberry Pi Pico SDK version 2.2.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

# mbedTLS for HTTPS to RetroAchievements; pico_mbedtls reads this when its
# target is created, so it has to come before pico_sdk_init()
set(PICO_MBEDTLS_CONFIG_FILE ${CMAKE_CURRENT_LIST_DIR}/game20k_mbedtls_config.h)

pico_sdk_init()

# Pull in FreeRTOS
include(./FreeRTOS_Kernel_import.cmake)

# u8g2
file(GLOB U8G2_SRC ../u8g2/csrc/*.c)
add_library(u8g2 ${U8G2_SRC})

# rcheevos (submodule), for the achievements of the game20k core
set(RCHEEVOS_DIR ${CMAKE_CURRENT_LIST_DIR}/../rcheevos)

# how this firmware introduces itself to RetroAchievements (User-Agent). The version here
# is the fallback for a build without the script: scripts/build_companion.sh of game20k
# passes its git describe output with -DGAME20K_VERSION=... instead. 0.0.0 so that such a
# build never looks like a release to the server.
if(NOT DEFINED GAME20K_VERSION)
  set(GAME20K_VERSION "0.0.0")
endif()
if(NOT DEFINED GAME20K_PLATFORM)
  set(GAME20K_PLATFORM "Tang Nano 20K")
endif()
# the game table, a C file of rows for ra_games.c. scripts/build_companion.sh of game20k
# passes the one it generates from its ROM manifests with -DGAME20K_GAMES_TABLE=...,
# without it the build takes the example, which holds no game.
if(NOT DEFINED GAME20K_GAMES_TABLE)
  set(GAME20K_GAMES_TABLE ${CMAKE_CURRENT_LIST_DIR}/../ra_games_example.c)
endif()
# it includes ra_games.h, which lies next to ra_games.c
set_source_files_properties(${GAME20K_GAMES_TABLE} PROPERTIES INCLUDE_DIRECTORIES ${CMAKE_CURRENT_LIST_DIR}/..)
message(STATUS "game20k: games table ${GAME20K_GAMES_TABLE}")
# the menus of game20k's cores, a C file for menus.c, passed the same way as
# -DGAME20K_MENU_TABLE=..., without it the build takes the example, which holds no menu
# of a core.
if(NOT DEFINED GAME20K_MENU_TABLE)
  set(GAME20K_MENU_TABLE ${CMAKE_CURRENT_LIST_DIR}/../menus_example.c)
endif()
set_source_files_properties(${GAME20K_MENU_TABLE} PROPERTIES INCLUDE_DIRECTORIES ${CMAKE_CURRENT_LIST_DIR}/..)
message(STATUS "game20k: menu table ${GAME20K_MENU_TABLE}")

add_executable(${PROJECT}
	../main.c
	mcu_hw.c
	bluetooth.c
	../sysctrl.c
	../hidparser.c
	../hid.c
	../sdc.c
	../osd_u8g2.c
	../menu.c
	../inifile.c
	../at_wifi.c
	../puff.c
	../fatfs/source/ff.c
	../fatfs/source/ffunicode.c
	../u8g2/sys/bitmap/common/u8x8_d_bitmap.c
	../tusb_xinput/xinput_host.c
	../tusb_asix/asix_host.c
	../config.c
	../freertos_callbacks.c
	../xml.c
	../ps2helper.c
	../ftpd.c
	../telnetd.c
	../ra_games.c
	${GAME20K_GAMES_TABLE}
	../menus.c
	${GAME20K_MENU_TABLE}
	../ra_patch.c
	../ra_net.c
	../ra_slim.c
	../ra_text.c
	../ra_task.c
	../ra_mac.c
	../ra_queue.c
	../ra_state.c
	../games_file.c
	../games.c
	# rcheevos: the runtime that evaluates the conditions and the reader for the
	# set (rapi), no rc_client. rc_validate.c stays out, only the editor needs it.
	${RCHEEVOS_DIR}/src/rcheevos/alloc.c
	${RCHEEVOS_DIR}/src/rcheevos/condition.c
	${RCHEEVOS_DIR}/src/rcheevos/condset.c
	${RCHEEVOS_DIR}/src/rcheevos/consoleinfo.c
	${RCHEEVOS_DIR}/src/rcheevos/format.c
	${RCHEEVOS_DIR}/src/rcheevos/lboard.c
	${RCHEEVOS_DIR}/src/rcheevos/memref.c
	${RCHEEVOS_DIR}/src/rcheevos/operand.c
	${RCHEEVOS_DIR}/src/rcheevos/richpresence.c
	${RCHEEVOS_DIR}/src/rcheevos/runtime.c
	${RCHEEVOS_DIR}/src/rcheevos/runtime_progress.c
	${RCHEEVOS_DIR}/src/rcheevos/trigger.c
	${RCHEEVOS_DIR}/src/rcheevos/value.c
	${RCHEEVOS_DIR}/src/rapi/rc_api_common.c
	${RCHEEVOS_DIR}/src/rapi/rc_api_runtime.c
	${RCHEEVOS_DIR}/src/rapi/rc_api_user.c
	${RCHEEVOS_DIR}/src/rc_util.c
	${RCHEEVOS_DIR}/src/rc_compat.c
	${RCHEEVOS_DIR}/src/rc_version.c
	${RCHEEVOS_DIR}/src/rhash/md5.c
)

family_add_pico_pio_usb(${PROJECT})

target_compile_definitions(${PROJECT} PRIVATE
        PICO_FLASH_ASSUME_CORE1_SAFE=1
        PIO_USB_DP_PIN_DEFAULT=2
        PICO_DEFAULT_PIO_USB_DP_PIN=2
	# CFG_TUSB_DEBUG=2
)

if(${BOARD} STREQUAL PICO)
   add_compile_definitions(${PROJECT} PRIVATE MISTLE_BOARD=0 ENABLE_WIFI ENABLE_BLUETOOTH)
   add_custom_command(TARGET ${PROJECT} POST_BUILD COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan "Firmware has been built for Raspberry Pi Pico or Pico-W.")
elseif(${BOARD} STREQUAL PICO2)
   # game20k: no Bluetooth on the Pico 2 W, the cabinet's stick is USB. Its 22 KB of RAM go
   # to the achievement sets: a large one with two leaderboards needs more than 140 KB.
   add_compile_definitions(${PROJECT} PRIVATE MISTLE_BOARD=1 ENABLE_WIFI)
   set(GAME20K_NO_BLUETOOTH ON)
   add_custom_command(TARGET ${PROJECT} POST_BUILD COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan "Firmware has been built for Raspberry Pi Pico2 or Pico2-W.")
elseif(${BOARD} STREQUAL WS2040ZERO)
   add_compile_definitions(${PROJECT} PRIVATE MISTLE_BOARD=2)
   target_sources(${PROJECT} PRIVATE ../jtag.c ../gowin.c usb_jtag.c)
   target_sources(${PROJECT} PRIVATE usb_descriptors.c)
   target_link_libraries(${PROJECT} PRIVATE tinyusb_device)
   add_custom_command(TARGET ${PROJECT} POST_BUILD COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan "Firmware has been built for Waveshare RP2040-Zero.")
elseif(${BOARD} STREQUAL SH20KLITE)
   add_compile_definitions(${PROJECT} PRIVATE MISTLE_BOARD=3)
   add_compile_definitions(${PROJECT} PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)
   add_custom_command(TARGET ${PROJECT} POST_BUILD COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan "Firmware has been built for MiSTeryShield20k-Lite.")
elseif(${BOARD} STREQUAL DEV20K)
   add_compile_definitions(${PROJECT} PRIVATE MISTLE_BOARD=4)
   add_compile_definitions(${PROJECT} PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)
   add_custom_command(TARGET ${PROJECT} POST_BUILD COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan "Firmware has been built for MiSTeryDev20k.")
   target_sources(${PROJECT} PRIVATE sdio.c ../jtag.c ../gowin.c usb_jtag.c)
   target_sources(${PROJECT} PRIVATE usb_descriptors.c)
   target_link_libraries(${PROJECT} PRIVATE tinyusb_device)
elseif(${BOARD} STREQUAL MSP20K)
   add_compile_definitions(${PROJECT} PRIVATE MISTLE_BOARD=5 ENABLE_WIFI ENABLE_BLUETOOTH)
   add_custom_command(TARGET ${PROJECT} POST_BUILD COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan "Firmware has been built for MiSTeryShieldPicoTN20k.")
   target_sources(${PROJECT} PRIVATE ../jtag.c ../gowin.c usb_jtag.c)
   target_sources(${PROJECT} PRIVATE usb_descriptors.c)
   target_link_libraries(${PROJECT} PRIVATE tinyusb_device)
elseif(${BOARD} STREQUAL DEV25K)
   add_compile_definitions(${PROJECT} PRIVATE MISTLE_BOARD=6)
   add_custom_command(TARGET ${PROJECT} POST_BUILD COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan "Firmware has been built for GW3A/GW5A Dev25k.")
   target_sources(${PROJECT} PRIVATE sdio.c ../jtag.c ../gowin.c usb_jtag.c)
   target_sources(${PROJECT} PRIVATE usb_descriptors.c)
   target_sources(${PROJECT} PRIVATE ../pio_jtag.c)
   target_link_libraries(${PROJECT} PRIVATE tinyusb_device)
   pico_generate_pio_header(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/jtag.pio)
else()
   message(FATAL_ERROR "BOARD must be PICO, PICO2, WS2040ZERO, SH20KLITE, DEV20K, MSP20K or DEV25K")
endif()

target_include_directories(${PROJECT} PRIVATE
      ${CMAKE_CURRENT_LIST_DIR}
      ${CMAKE_CURRENT_LIST_DIR}/../fatfs/source
      ${CMAKE_CURRENT_LIST_DIR}/../u8g2/csrc
      ${CMAKE_CURRENT_LIST_DIR}/../tusb_xinput
      ${CMAKE_CURRENT_LIST_DIR}/../tusb_asix
)

string(APPEND CMAKE_C_FLAGS " -Os -fdata-sections -ffunction-sections -Wno-error=incompatible-pointer-types" )

target_link_libraries(${PROJECT} PRIVATE pico_stdlib pico_multicore)
target_link_libraries(${PROJECT} PRIVATE FreeRTOS-Kernel FreeRTOS-Kernel-Heap4)
target_link_libraries(${PROJECT} PRIVATE hardware_pio hardware_dma hardware_spi hardware_adc)
# rcheevos, without threads
target_include_directories(${PROJECT} PRIVATE ${RCHEEVOS_DIR}/include ${RCHEEVOS_DIR}/src)
target_compile_definitions(${PROJECT} PRIVATE RC_NO_THREADS=1)
target_compile_definitions(${PROJECT} PRIVATE
	GAME20K_VERSION="${GAME20K_VERSION}" GAME20K_PLATFORM="${GAME20K_PLATFORM}")
# malloc returns NULL when the SDK heap is out instead of stopping the firmware. How much a
# set takes is up to the server, beyond our reach: what finds no room beside the parsed set
# comes once it is freed, else it stays off and hardcore is blocked (ra_patch.c), a request
# that finds too little fails and is asked again, and the game goes on. Our own mallocs
# check for NULL. A large set once stopped the firmware at every start.
target_compile_definitions(${PROJECT} PRIVATE PICO_MALLOC_PANIC=0)
target_link_libraries(${PROJECT} PRIVATE pico_cyw43_arch_lwip_sys_freertos)
if(NOT GAME20K_NO_BLUETOOTH)
  target_link_libraries(${PROJECT} PRIVATE pico_btstack_cyw43 pico_btstack_classic)
endif()
target_link_libraries(${PROJECT} PRIVATE tinyusb_host tinyusb_board u8g2)
target_link_libraries(${PROJECT} PRIVATE pico_lwip_sntp)
# HTTPS to RetroAchievements: the HTTP client, lwIP's TLS layer, mbedTLS
target_link_libraries(${PROJECT} PRIVATE pico_lwip_http pico_lwip_mbedtls pico_mbedtls)

pico_add_extra_outputs(${PROJECT})
pico_enable_stdio_usb(${PROJECT} 0)
pico_enable_stdio_uart(${PROJECT} 1)

add_custom_target(flash
    COMMAND echo "Flashing ${PROJECT} ..."
    COMMAND openocd -f interface/cmsis-dap.cfg -f target/${TARGET}.cfg -c "adapter speed 5000" -c "program ${PROJECT}.elf verify reset exit"
    DEPENDS "${PROJECT}"
    COMMENT "Flash target using openocd"
)

target_link_options(${PROJECT} PRIVATE -Xlinker --print-memory-usage)
target_compile_options(${PROJECT} PRIVATE -Wall -Wextra)
	
add_custom_target(reset
    COMMAND echo "Reseting ${PROJECT} for ${TARGET} ..."
    COMMAND openocd -f interface/cmsis-dap.cfg -f target/${TARGET}.cfg -c init -c reset -c exit
    COMMENT "Reset target using openocd"
)

if(${BOARD} STREQUAL WS2040ZERO)
  add_custom_target(term COMMAND term.sh /dev/ttyACM0 460800)
else()
  add_custom_target(term COMMAND term.sh /dev/ttyACM0 921600)
endif()
