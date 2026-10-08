# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Bare-metal ARM toolchain file.  CubeCLT ships the compiler; nothing is
# fetched at build time (UR-CON-04).  Override with -DTOOLCHAIN_PREFIX_PATH if it
# lives elsewhere.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT TOOLCHAIN_PREFIX_PATH)
    file(GLOB _clt /opt/St/STM32CubeCLT*/GNU-tools-for-STM32/bin)
    list(GET _clt 0 TOOLCHAIN_PREFIX_PATH)
endif()

find_program(CMAKE_C_COMPILER   arm-none-eabi-gcc HINTS ${TOOLCHAIN_PREFIX_PATH})
find_program(CMAKE_CXX_COMPILER arm-none-eabi-g++ HINTS ${TOOLCHAIN_PREFIX_PATH})
find_program(CMAKE_ASM_COMPILER arm-none-eabi-gcc HINTS ${TOOLCHAIN_PREFIX_PATH})
find_program(CMAKE_OBJCOPY      arm-none-eabi-objcopy HINTS ${TOOLCHAIN_PREFIX_PATH})
find_program(CMAKE_SIZE         arm-none-eabi-size    HINTS ${TOOLCHAIN_PREFIX_PATH})

# A freestanding compiler cannot link a test binary, so do not try.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
