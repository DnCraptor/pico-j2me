# Nano Java for RP2350 boards (Murmulator 1.x / 2.0, Olimex RP2350-PICO-PC,
# Waveshare RP2350-PiZero) with QSPI PSRAM.
#
#   cmake -B build-rp2350 -DPICO_BOARD=murmulator2 -DHDMI=ON
#   cmake --build build-rp2350
#
# The desktop build used by the tests is the Makefile (make host / make test).

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13...3.23)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_PLATFORM "rp2350" CACHE STRING "Chip type")
set(PICO_BOARD "murmulator2" CACHE STRING "Board type")

option(VGA "VGA output" OFF)
option(HDMI "HDMI output" OFF)
option(I2S "Sound on an I2S DAC (OFF: PWM)" ON)
option(I2S_CS4334 "I2S DAC is a CS4334" OFF)
option(USB_HID "USB keyboard on the native USB port (TinyUSB host)" ON)
set(CPU_MHZ "" CACHE STRING "clk_sys in MHz (empty: the board's choice, else 378)")

if(${PICO_BOARD} MATCHES "waveshare_rp2350_pizero")
    # RP2350B, 16 MB flash, HDMI on GPIO32..39, PCM5102A on I2S
    set(PICO_FLASH_SIZE_BYTES 16777216 CACHE STRING "" FORCE)
    set(HDMI ON CACHE BOOL "HDMI output" FORCE)
    set(VGA OFF CACHE BOOL "VGA output" FORCE)
endif()
if(NOT VGA AND NOT HDMI)
    set(HDMI ON)
endif()

include(pico_sdk_import.cmake)

project(pico-j2me C CXX ASM)
set(PICO_J2ME_VERSION "0.1.0")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
# No -ffast-math: Java floating point needs NaN and infinity.
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections -O2")

pico_sdk_init()

# The UF2 files are also copied to bin/<build type>/ (see the end).
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")

add_subdirectory(drivers/fatfs)
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/ps2kbd)
add_subdirectory(drivers/nespad)
add_subdirectory(drivers/audio)
add_subdirectory(drivers/vga-nextgen)
add_subdirectory(drivers/hdmi)
add_subdirectory(drivers/graphics)

# ---------------------------------------------------------------------------
# The class library: built with a JDK 8 javac (javac 9+ cannot target the
# CLDC class file version), otherwise the prebuilt copy in classlib/ is used.

file(GLOB_RECURSE CLASSLIB_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/classlib/src/*.java)
set(CLASSLIB_PREBUILT ${CMAKE_SOURCE_DIR}/classlib/classlib.jar)
set(CLASSLIB_JAR ${CLASSLIB_PREBUILT})

find_program(JAVAC_EXECUTABLE javac HINTS $ENV{JAVA8_HOME}/bin $ENV{JAVA_HOME}/bin)
find_program(JAR_EXECUTABLE jar HINTS $ENV{JAVA8_HOME}/bin $ENV{JAVA_HOME}/bin)
set(JAVAC_IS_8 OFF)
if(JAVAC_EXECUTABLE AND JAR_EXECUTABLE)
    execute_process(COMMAND ${JAVAC_EXECUTABLE} -version
                    OUTPUT_VARIABLE _javac_out ERROR_VARIABLE _javac_out)
    if(_javac_out MATCHES "javac 1\\.[5-8]")
        set(JAVAC_IS_8 ON)
    endif()
endif()
if(JAVAC_IS_8)
    set(CLASSLIB_JAR ${CMAKE_BINARY_DIR}/classlib.jar)
    set(_cl_dir ${CMAKE_BINARY_DIR}/classlib-classes)
    string(REPLACE ";" "\n" _cl_list "${CLASSLIB_SOURCES}")
    file(WRITE ${CMAKE_BINARY_DIR}/classlib-sources.txt "${_cl_list}\n")
    add_custom_command(
        OUTPUT ${CLASSLIB_JAR}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_cl_dir}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_cl_dir}
        COMMAND ${JAVAC_EXECUTABLE} -nowarn -Xlint:-options -encoding UTF-8 -source 1.3 -target 1.3
                -bootclasspath ${_cl_dir} -extdirs "" -d ${_cl_dir} @${CMAKE_BINARY_DIR}/classlib-sources.txt
        COMMAND ${JAR_EXECUTABLE} cfM ${CLASSLIB_JAR} -C ${_cl_dir} .
        DEPENDS ${CLASSLIB_SOURCES}
        COMMENT "javac classlib"
        VERBATIM)
    message(STATUS "classlib: built with ${JAVAC_EXECUTABLE}")
else()
    message(STATUS "classlib: no JDK 8 javac found (JAVA8_HOME), using classlib/classlib.jar")
endif()
add_custom_target(classlib DEPENDS ${CLASSLIB_JAR})

# ---------------------------------------------------------------------------

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS
    src/vm/*.c src/util/*.c src/midp/*.c src/audio/*.c)
set(PLATFORM_SOURCES
    platform/rp2350/main.c
    platform/rp2350/pal_rp2350.c
    platform/rp2350/sys_rp2350.c
    platform/rp2350/video.c
    platform/rp2350/audio_rp2350.c
    platform/rp2350/settings.c
    platform/rp2350/ui.c
    platform/rp2350/input.cpp
    platform/rp2350/hid_app.cpp
    platform/rp2350/psram.c
)

add_executable(${PROJECT_NAME}
    ${CORE_SOURCES}
    ${PLATFORM_SOURCES}
    external/tlsf/tlsf.c
    platform/rp2350/classlib.S
)
add_dependencies(${PROJECT_NAME} classlib)
set_source_files_properties(platform/rp2350/classlib.S PROPERTIES
    COMPILE_DEFINITIONS "CLASSLIB_JAR=\"${CLASSLIB_JAR}\""
    OBJECT_DEPENDS ${CLASSLIB_JAR})
# Our own code is kept free of warnings.
set_source_files_properties(${CORE_SOURCES} ${PLATFORM_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter;-Wno-missing-field-initializers")

string(TIMESTAMP NJ_BUILD_EPOCH "%s" UTC)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    NJ_VERSION="${PICO_J2ME_VERSION}"
    NJ_BUILD_EPOCH=${NJ_BUILD_EPOCH}LL
    # core 0 runs the VM: the interpreter and the collector recurse little,
    # but the class loader and the natives use some stack
    PICO_STACK_SIZE=0x4000
    PICO_CORE1_STACK_SIZE=0x800
)
if(CPU_MHZ)
    target_compile_definitions(${PROJECT_NAME} PRIVATE CPU_MHZ=${CPU_MHZ})
endif()

# The core 0 stack moves to the main RAM (platform/rp2350/memmap.ld).
pico_set_linker_script(${PROJECT_NAME} ${CMAKE_SOURCE_DIR}/platform/rp2350/memmap.ld)

target_include_directories(${PROJECT_NAME} PRIVATE
    platform/rp2350
    external/tlsf
)

if(${PICO_BOARD} MATCHES "murmulator2")
    set(BUILD_NAME "m2p2")
elseif(${PICO_BOARD} MATCHES "olimex-pico-pc")
    set(BUILD_NAME "PCp2")
elseif(${PICO_BOARD} MATCHES "waveshare_rp2350_pizero")
    set(BUILD_NAME "z0p2")
else()
    set(BUILD_NAME "m1p2")
endif()
set(BUILD_NAME "${BUILD_NAME}-j2me")

if(HDMI)
    target_link_libraries(${PROJECT_NAME} PRIVATE hdmi)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HDMI)
    set(BUILD_NAME "${BUILD_NAME}-HDMI")
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE vga-nextgen)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VGA)
    set(BUILD_NAME "${BUILD_NAME}-VGA")
endif()

if(NOT I2S)
    target_compile_definitions(${PROJECT_NAME} PRIVATE AUDIO_PWM)
    set(BUILD_NAME "${BUILD_NAME}-PWM")
elseif(I2S_CS4334)
    target_compile_definitions(${PROJECT_NAME} PRIVATE I2S_CS4334)
    set(BUILD_NAME "${BUILD_NAME}-I2S-CS4334")
else()
    set(BUILD_NAME "${BUILD_NAME}-I2S")
endif()

if(USB_HID)
    target_compile_definitions(${PROJECT_NAME} PRIVATE KBDUSB)
    target_link_libraries(${PROJECT_NAME} PRIVATE tinyusb_host)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE tinyusb_device)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    graphics
    audio
    ps2kbd
    nespad
    sdcard
    fatfs
    pico_stdlib
    pico_multicore
    hardware_dma
    hardware_pio
    hardware_pwm
    tinyusb_board
)

pico_set_program_name(${PROJECT_NAME} "Nano Java: J2ME (MIDP 2.0) for RP2350")
pico_set_program_version(${PROJECT_NAME} "${PICO_J2ME_VERSION}")
pico_enable_stdio_uart(${PROJECT_NAME} 0)
pico_enable_stdio_usb(${PROJECT_NAME} 0)
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${BUILD_NAME}-${PICO_J2ME_VERSION}")
target_link_options(${PROJECT_NAME} PRIVATE -Xlinker --print-memory-usage)
pico_add_extra_outputs(${PROJECT_NAME})
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTPUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/${BUILD_NAME}-${PICO_J2ME_VERSION}.uf2 ${OUTPUT_DIR}/
    VERBATIM)
