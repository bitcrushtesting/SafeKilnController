# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The supervisor's target build: STM32G031K8, arm-none-eabi, no vendor HAL.
#
#   cmake -B build-sup-target -S supervisor \
#         -DCMAKE_TOOLCHAIN_FILE=firmware/supervisor/arm-none-eabi.cmake
#   cmake --build build-sup-target
#
# The host tests are a separate project under test/host and need no ARM
# toolchain, because everything that decides anything is platform-free.
cmake_minimum_required(VERSION 3.16)
project(kiln_supervisor C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(supervisor.elf
    src/main.cpp
    board/startup.cpp
    core/src/trip.cpp
    core/src/max31856.cpp
    protocol/src/sup_proto.cpp)

target_include_directories(supervisor.elf PRIVATE
    board core/include protocol/include)

# Freestanding and bounded: no exceptions, no RTTI, no heap, and -Wall -Wextra
# -Werror as everywhere else in this repository.  AD-20's profile applies here
# too; the supervisor is held to the same standard as the code it supervises.
target_compile_options(supervisor.elf PRIVATE
    -mcpu=cortex-m0plus -mthumb
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wdouble-promotion
    -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
    -ffunction-sections -fdata-sections
    -fno-common -fstack-usage
    -Os -g3)

target_link_options(supervisor.elf PRIVATE
    -mcpu=cortex-m0plus -mthumb
    -T${CMAKE_CURRENT_LIST_DIR}/board/stm32g031k8.ld
    # -nostartfiles, not -nostdlib: the Cortex-M0+ has no FPU, so every float
    # compare and multiply is a libgcc soft-float helper call, and -nostdlib
    # takes libgcc away along with the C library.  newlib-nano supplies
    # lroundf.  Tasklist R10 proposes removing the floats entirely, which would
    # let this go back to -nostdlib.
    -nostartfiles --specs=nano.specs
    -Wl,--gc-sections,--print-memory-usage
    -Wl,-Map=supervisor.map)

# A supervisor that outgrows a part this size has stopped being a supervisor.
# The real budget is reviewability, but a hard number catches drift.
add_custom_command(TARGET supervisor.elf POST_BUILD
    COMMAND ${CMAKE_SIZE} supervisor.elf)
