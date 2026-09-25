# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Standalone probe; not part of the shadPS4 build.
cmake_minimum_required(VERSION 3.20)
project(e0b_uma_probe CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "e0b_uma_probe needs Linux (memfd, udmabuf, dma-buf)")
endif()

find_package(Vulkan REQUIRED)

add_executable(e0b_uma_probe
    src/caps.cpp
    src/common.cpp
    src/env.cpp
    src/functional.cpp
    src/guest_mem.cpp
    src/main.cpp
    src/probe.cpp
    src/report.cpp
    src/vk_ctx.cpp
)
target_link_libraries(e0b_uma_probe PRIVATE Vulkan::Vulkan)
target_compile_options(e0b_uma_probe PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
