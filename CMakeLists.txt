cmake_minimum_required(VERSION 3.14)
#FreeRTOS from https://github.com/raspberrypi/FreeRTOS-kernel

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 13_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")
set(PICO_PLATFORM rp2040)

# Set default build type to Debug if not specified.
# NOTE: deliberately NOT using FORCE here - FORCE would overwrite a build type
# passed on the command line (-DCMAKE_BUILD_TYPE=Release), making Release
# impossible to select.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build (Debug or Release)")
endif()

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

# Name the project
project(RC422Dev
    LANGUAGES C CXX ASM
    DESCRIPTION "RetroCore RS-422 Device FreeRTOS runtime)"
)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()


#### FreeRTOS #####
# NOTE: the directory name must match the `path` in .gitmodules EXACTLY
# (lowercase "kernel"). Windows/NTFS is case-insensitive so a mismatch works
# here, but a fresh clone on Linux/CI creates the lowercase name and any
# capitalised path would not resolve.
set(FREERTOS_KERNEL_PATH "${CMAKE_SOURCE_DIR}/external/FreeRTOS-kernel")

add_library(freertos_config INTERFACE)

# The config file directory - this is what provides FreeRTOSConfig.h from ./include
target_include_directories(freertos_config SYSTEM INTERFACE
    include
    ${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040
)

# Selects the port from PICO_PLATFORM and calls add_subdirectory() on it, which
# pulls in library.cmake - that is what defines the FreeRTOS-Kernel and
# FreeRTOS-Kernel-Heap4 INTERFACE targets linked below.
# NOTE: do NOT also add_subdirectory(${FREERTOS_KERNEL_PATH}) (the kernel root).
# The root builds a SECOND, separate copy of the kernel (libfreertos_kernel.a)
# using the generic portable/GCC/ARM_CM0 port instead of the RP2040 port. That
# copy is never linked into the firmware - it is compiled and discarded. The
# root is also the only consumer of FREERTOS_PORT, which is why no
# set(FREERTOS_PORT ...) is needed here.
if ("${PICO_PLATFORM}" STREQUAL "rp2040")
    include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake)
elseif("${PICO_PLATFORM}" STREQUAL "rp2350-arm-s")
    include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake)
elseif("${PICO_PLATFORM}" STREQUAL "rp2350-riscv")
    include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2350_RISC-V/FreeRTOS_Kernel_import.cmake)
endif()

#Include app source code
#add_subdirectory(src)


# Explicit source list. Previously this was a file(GLOB ...), which CMake only
# evaluates at configure time - a newly added .cpp/.c was silently ignored until
# someone remembered to reconfigure. Listing the files means a new source is a
# build error until it is added here, which is the failure mode we want.
# NOTE: src/backup/ is intentionally absent - it is frozen reference code.
set(SOURCES
    src/commands.cpp
    src/console.cpp
    src/functions.cpp
    src/hdlc.cpp
    src/main.cpp
    src/RingBuffer.cpp
    src/shared.cpp
    src/sync.cpp
)

# Add executable target
add_executable(RC422Dev
    ${SOURCES}
)


# Generate PIO header for sync receice and transmit
# Mst be after add_executablei
pico_generate_pio_header(RC422Dev ${CMAKE_CURRENT_LIST_DIR}/src/sdlc.pio)


# NOTE: no "../include" here - that resolved OUTSIDE the repo and did not exist.
# FreeRTOSConfig.h lives in ./include and reaches the compiler via the
# freertos_config INTERFACE target linked below.
target_include_directories(RC422Dev PRIVATE
    ${FREERTOS_KERNEL_PATH}/include
    ${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040
)


# Link to built libraries
target_link_libraries(RC422Dev LINK_PUBLIC
    pico_stdlib
    pico_multicore
    hardware_clocks      # Hardware Clocks Library
    hardware_pio         # Hardware PIO
    hardware_dma

    freertos_config
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4 
    )
    
#FreeRTOS heap choices
# https://www.freertos.org/Documentation/02-Kernel/02-Kernel-features/09-Memory-management/01-Memory-management
#
# heap_1 - the very simplest, does not permit memory to be freed.
# heap_2 - permits memory to be freed, but does not coalescence adjacent free blocks.
# heap_3 - simply wraps the standard malloc() and free() for thread safety.
# heap_4 - coalescences adjacent free blocks to avoid fragmentation. Includes absolute address placement option.
# heap_5 - as per heap_4, with the ability to span the heap across multiple non-adjacent memory areas.
#
#Notes:
#  heap_1 is less useful since FreeRTOS added support for static allocation.
#  heap_2 is now considered legacy as the newer heap_4 implementation is preferred.

# Enable/disable STDIO via USB and UART
pico_enable_stdio_usb(RC422Dev  1)
pico_enable_stdio_uart(RC422Dev 0)


# Run the entire project in SRAM
# pico_set_binary_type(RC422Dev copy_to_ram)

# Enable extra build products
pico_add_extra_outputs(RC422Dev)



