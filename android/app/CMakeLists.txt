cmake_minimum_required(VERSION 3.18)
project(linksaver C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDL_DIR} sdl2)

file(GLOB GAME_SRCS ${ROOT}/src/*.c ${ROOT}/src/linksaver/*.c ${ROOT}/snes/*.c)
add_library(main SHARED ${GAME_SRCS}
  ${ROOT}/third_party/gl_core/gl_core_3_1.c
  ${ROOT}/third_party/opus-1.3.1-stripped/opus_decoder_amalgam.c)
target_include_directories(main PRIVATE ${ROOT} ${ROOT}/src)
target_compile_definitions(main PRIVATE SYSTEM_VOLUME_MIXER_AVAILABLE=0 NDEBUG)
# The game was written for x86, where plain char is signed. It is unsigned on ARM.
target_compile_options(main PRIVATE -O2 -fno-strict-aliasing -fsigned-char
  -include ${CMAKE_CURRENT_SOURCE_DIR}/android_log.h)
target_link_libraries(main PRIVATE SDL2 m log)
