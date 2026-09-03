cmake_minimum_required(VERSION 3.10)
project(luansha_rpg C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

file(GLOB LUANSHA_SOURCES
  "src/*.c"
)

add_executable(luansha ${LUANSHA_SOURCES})

if(WIN32)
  target_link_libraries(luansha ws2_32 shell32)
endif()

if(MSVC)
  target_compile_options(luansha PRIVATE /utf-8)
endif()