cmake_minimum_required(VERSION 3.20)
set(PICO_BOARD pimoroni_tufty2350 CACHE STRING "Native badge hardware")
set(PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards")
if(NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
endif()
if(NOT EXISTS "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    message(FATAL_ERROR "Set PICO_SDK_PATH to Pico SDK 2.2.0 with its submodules")
endif()
include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
project(badge_video_streamer C CXX ASM)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()
if(NOT PICO_SDK_VERSION_STRING STREQUAL "2.2.0")
    message(FATAL_ERROR "This receiver and its linker adaptation require Pico SDK 2.2.0")
endif()
option(NATIVE_RGB565_LE "Native RGB565 memory order with 16-bit LCD DMA" ON)
option(NATIVE_JPEG_DSP "Corrected JPEGDEC M33 DSP path (requires LE)" ON)
option(NATIVE_JPEG_SRAM "Hot JPEG decoder code/table placement in SRAM" ON)
option(NATIVE_CYW43_BOUNDED_TX "8 ms data-only CYW43 flow-control wait" ON)
if(NATIVE_JPEG_DSP AND NOT NATIVE_RGB565_LE)
    message(FATAL_ERROR "NATIVE_JPEG_DSP requires NATIVE_RGB565_LE")
endif()
if(NATIVE_CYW43_BOUNDED_TX)
    if(NOT TARGET cyw43_driver)
        message(FATAL_ERROR "NATIVE_CYW43_BOUNDED_TX requires the SDK CYW43 driver")
    endif()
    set(native_cyw43_original "${PICO_CYW43_DRIVER_PATH}/src/cyw43_ll.c")
    set(native_cyw43_bounded "${CMAKE_CURRENT_BINARY_DIR}/cyw43_bounded/cyw43_ll.c")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/prepare_cyw43_tx.py"
        "${native_cyw43_original}" "${native_cyw43_bounded}" COMMAND_ERROR_IS_FATAL ANY)
    get_target_property(native_cyw43_sources cyw43_driver INTERFACE_SOURCES)
    set(native_cyw43_replacements 0)
    set(native_cyw43_staged_sources)
    foreach(driver_source IN LISTS native_cyw43_sources)
        if(driver_source STREQUAL native_cyw43_original)
            list(APPEND native_cyw43_staged_sources "${native_cyw43_bounded}")
            math(EXPR native_cyw43_replacements "${native_cyw43_replacements} + 1")
        else()
            list(APPEND native_cyw43_staged_sources "${driver_source}")
        endif()
    endforeach()
    if(NOT native_cyw43_replacements EQUAL 1)
        message(FATAL_ERROR "SDK CYW43 source list changed; review the build-local data-TX adaptation")
    endif()
    set_property(TARGET cyw43_driver PROPERTY INTERFACE_SOURCES "${native_cyw43_staged_sources}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${native_cyw43_original}" "${CMAKE_CURRENT_LIST_DIR}/prepare_cyw43_tx.py")
endif()

add_library(native_badge_display STATIC display.cpp)
target_include_directories(native_badge_display PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(native_badge_display PUBLIC NATIVE_RGB565_LE=$<BOOL:${NATIVE_RGB565_LE}>)
pico_generate_pio_header(native_badge_display "${CMAKE_CURRENT_LIST_DIR}/st7789_parallel.pio")
target_link_libraries(native_badge_display PUBLIC pico_stdlib hardware_pio hardware_dma hardware_pwm hardware_clocks)
target_compile_options(native_badge_display PRIVATE -O2 -Wall -Wextra -Werror)

function(native_outputs target)
    target_compile_definitions(${target} PRIVATE
        PICO_STDIO_USB_STDOUT_TIMEOUT_US=0 CFG_TUD_CDC_TX_BUFSIZE=1024)
    pico_enable_stdio_usb(${target} 1)
    pico_enable_stdio_uart(${target} 0)
    # SDK's default RP2350-E10 block at 0x10ffff00 overlaps Badgeware storage.
    # Keep every UF2 write below 2 MiB, including this compatibility block.
    set_property(TARGET ${target} PROPERTY PICOTOOL_EXTRA_UF2_ARGS --abs-block 0x101fff00)
    pico_add_extra_outputs(${target})
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_uf2.py"
                "${CMAKE_CURRENT_BINARY_DIR}/${target}.uf2"
        VERBATIM)
endfunction()

add_executable(native_display_smoke display_smoke.cpp)
target_link_libraries(native_display_smoke PRIVATE native_badge_display)
native_outputs(native_display_smoke)

set(JPEGDEC_PATH "$ENV{JPEGDEC_PATH}" CACHE PATH "Directory containing JPEGDEC.cpp and JPEGDEC.h")
if(EXISTS "${JPEGDEC_PATH}/JPEGDEC.cpp")
    set(native_jpegdec_path "${JPEGDEC_PATH}")
    if(NATIVE_JPEG_DSP OR NATIVE_JPEG_SRAM)
        set(native_jpegdec_path "${CMAKE_CURRENT_BINARY_DIR}/jpegdec_prepared")
        set(native_jpegdec_flags)
        if(NATIVE_JPEG_DSP)
            list(APPEND native_jpegdec_flags --dsp)
        endif()
        if(NATIVE_JPEG_SRAM)
            list(APPEND native_jpegdec_flags --sram)
        endif()
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/prepare_jpegdec.py"
            "${JPEGDEC_PATH}" "${native_jpegdec_path}" ${native_jpegdec_flags} COMMAND_ERROR_IS_FATAL ANY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${JPEGDEC_PATH}/JPEGDEC.cpp" "${JPEGDEC_PATH}/JPEGDEC.h" "${JPEGDEC_PATH}/jpeg.inl"
            "${CMAKE_CURRENT_LIST_DIR}/prepare_jpegdec.py")
    endif()
    add_library(native_jpegdec STATIC "${native_jpegdec_path}/JPEGDEC.cpp")
    target_include_directories(native_jpegdec PUBLIC "${native_jpegdec_path}")
    target_link_libraries(native_jpegdec PUBLIC pico_stdlib)
    target_compile_options(native_jpegdec PRIVATE -O2)
    if(NATIVE_JPEG_DSP)
        # Only the decoder translation unit sees the M33/CMSIS adapter; none
        # of the SDK sources gain a different ARM architecture declaration.
        set_source_files_properties("${native_jpegdec_path}/JPEGDEC.cpp" PROPERTIES
            COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/jpeg_arm_dsp.hpp")
    endif()
    add_library(native_jpeg_decode STATIC jpeg_decode.cpp)
    target_link_libraries(native_jpeg_decode PUBLIC native_jpegdec)
    target_include_directories(native_jpeg_decode PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_definitions(native_jpeg_decode PUBLIC
        NATIVE_RGB565_LE=$<BOOL:${NATIVE_RGB565_LE}>
        NATIVE_JPEG_DSP=$<BOOL:${NATIVE_JPEG_DSP}>
        NATIVE_JPEG_SRAM=$<BOOL:${NATIVE_JPEG_SRAM}>)
    target_compile_options(native_jpeg_decode PRIVATE -O2 -Wall -Wextra -Werror)

    set(NATIVE_JPEG_QUALITY 6 CACHE STRING "JPEG quality, 4 through 63 (lower preserves more detail)")
    option(NATIVE_JPEG_PSRAM "Two-slot uncached PSRAM compressed receiver" ON)
    set(NATIVE_PREFETCH_LEAD_US 12000 CACHE STRING "Next-frame credit lead, 0 through 12000 us")
    if(NOT NATIVE_PREFETCH_LEAD_US MATCHES "^(0|[1-9][0-9]*)$" OR NATIVE_PREFETCH_LEAD_US GREATER 12000)
        message(FATAL_ERROR "NATIVE_PREFETCH_LEAD_US must be an integer from 0 through 12000")
    endif()
    if(NOT NATIVE_JPEG_PSRAM AND NOT NATIVE_PREFETCH_LEAD_US EQUAL 0)
        message(FATAL_ERROR "Nonzero NATIVE_PREFETCH_LEAD_US requires NATIVE_JPEG_PSRAM=ON")
    endif()
    set(NATIVE_TCP_TIMER_MS 10 CACHE STRING "lwIP TCP fast timer interval, 5 through 250 ms")
    if(NOT NATIVE_TCP_TIMER_MS MATCHES "^[1-9][0-9]*$" OR
            NATIVE_TCP_TIMER_MS LESS 5 OR NATIVE_TCP_TIMER_MS GREATER 250)
        message(FATAL_ERROR "NATIVE_TCP_TIMER_MS must be an integer from 5 through 250")
    endif()
    option(NATIVE_DIAGNOSTICS "Collect full timing distributions and USB diagnostic records" OFF)
    option(NATIVE_TRACE_GAPS "Emit bounded correlated frame traces after long video gaps" OFF)
    option(NATIVE_TRACE_TCP_INGRESS "Observe TCP segment order before ordered delivery (requires gap trace)" OFF)
    if(NATIVE_TRACE_GAPS AND NOT NATIVE_DIAGNOSTICS)
        message(FATAL_ERROR "NATIVE_TRACE_GAPS requires NATIVE_DIAGNOSTICS=ON")
    endif()
    if(NATIVE_TRACE_TCP_INGRESS AND NOT NATIVE_TRACE_GAPS)
        message(FATAL_ERROR "NATIVE_TRACE_TCP_INGRESS requires NATIVE_TRACE_GAPS=ON")
    endif()
    add_executable(native_video_streamer main.cpp stream_client.cpp hud.cpp)
    if(NATIVE_JPEG_PSRAM)
        target_sources(native_video_streamer PRIVATE psram.cpp)
    endif()
    target_include_directories(native_video_streamer PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/../esp/components/protocol/include")
    target_compile_definitions(native_video_streamer PRIVATE
        NATIVE_JPEG_QUALITY=${NATIVE_JPEG_QUALITY}
        NATIVE_DIAGNOSTICS=$<BOOL:${NATIVE_DIAGNOSTICS}>
        NATIVE_CYW43_BOUNDED_TX=$<BOOL:${NATIVE_CYW43_BOUNDED_TX}>
        NATIVE_TRACE_GAPS=$<BOOL:${NATIVE_TRACE_GAPS}>
        NATIVE_TRACE_TCP_INGRESS=$<BOOL:${NATIVE_TRACE_TCP_INGRESS}>
        NATIVE_TCP_TIMER_MS=${NATIVE_TCP_TIMER_MS}
        NATIVE_JPEG_PSRAM=$<BOOL:${NATIVE_JPEG_PSRAM}>
        NATIVE_PREFETCH_LEAD_US=${NATIVE_PREFETCH_LEAD_US}
        PICO_STACK_SIZE=0x2000 PICO_CORE1_STACK_SIZE=0x800)
    target_compile_options(native_video_streamer PRIVATE -O2 -Wall -Wextra -Werror)
    target_link_libraries(native_video_streamer PRIVATE native_badge_display native_jpeg_decode
        pico_cyw43_arch_lwip_poll pico_multicore pico_util pico_bootrom hardware_watchdog)
    # The SDK default puts core 0's stack in a 4 KiB scratch bank. Networking
    # and bounded JSON formatting need 8 KiB, so reserve it at main SRAM's top.
    # Generate from the pinned SDK instead of duplicating its complete linker
    # script; fail if a different SDK changes any of the expected seams.
    file(READ "${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2350/memmap_default.ld" native_linker)
    set(old_stack_section ".stack_dummy (NOLOAD):\n    {\n        KEEP(*(.stack*))\n    } > SCRATCH_Y")
    set(new_stack_section ".stack_dummy (NOLOAD):\n    {\n        KEEP(*(.stack*))\n    } > RAM")
    set(old_stack_top "__StackTop = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y);")
    set(new_stack_top "__StackTop = ORIGIN(RAM) + LENGTH(RAM);")
    set(old_stack_limit "__StackLimit = ORIGIN(RAM) + LENGTH(RAM);")
    set(new_stack_limit "__StackLimit = ORIGIN(RAM) + LENGTH(RAM) - SIZEOF(.stack_dummy);")
    set(old_heap_limit "__HeapLimit = ORIGIN(RAM) + LENGTH(RAM);")
    set(new_heap_limit "__HeapLimit = ORIGIN(RAM) + LENGTH(RAM) - SIZEOF(.stack_dummy);")
    set(old_heap_assert "ASSERT(__StackLimit >= __HeapLimit, \"region RAM overflowed\")")
    set(new_heap_assert "ASSERT(__StackLimit >= __HeapLimit, \"region RAM overflowed\")\n    ASSERT(__HeapLimit >= __end__ + SIZEOF(.heap), \"native stack overlaps globals/minimum heap\")")
    foreach(seam IN ITEMS stack_section stack_top stack_limit heap_limit heap_assert)
        string(FIND "${native_linker}" "${old_${seam}}" found_seam)
        if(found_seam LESS 0)
            message(FATAL_ERROR "SDK linker script changed at ${seam}; review the main-SRAM stack adaptation")
        endif()
        string(REPLACE "${old_${seam}}" "${new_${seam}}" native_linker "${native_linker}")
    endforeach()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/native_memmap.ld" "${native_linker}")
    pico_set_linker_script(native_video_streamer "${CMAKE_CURRENT_BINARY_DIR}/native_memmap.ld")
    native_outputs(native_video_streamer)
else()
    message(STATUS "JPEGDEC_PATH is unset: only native_display_smoke is available")
endif()
