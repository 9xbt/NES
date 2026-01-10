cmake_minimum_required (VERSION 3.5)
project (nes_emu)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_C_FLAGS "-Wall -Werror")
set(CMAKE_CXX_FLAGS "-Wall -Werror")

#version number
set (nes_emu_VERSION_MAJOR 0)
set (nes_emu_VERSION_MINOR 0)

set(CMAKE_C_FLAGS_RELEASE "-O3 -g")

find_package(X11 REQUIRED)
include_directories(nes_emu ${X11_INCLUDE_DIR})

include_directories(nes)

#indicate the entry point for the executable
add_executable (nes_emu main.c nes/cpu.c nes/cartridge.c nes/ppu.c nes/controller.c nes/memory_controller.c)

target_link_libraries(nes_emu ${X11_LIBRARIES} rt)

install (TARGETS nes_emu DESTINATION bin)