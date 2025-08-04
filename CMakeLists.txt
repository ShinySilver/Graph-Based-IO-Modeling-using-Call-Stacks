cmake_minimum_required(VERSION 3.20)
project (griot-basic C)

set(bin ${CMAKE_CURRENT_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${bin})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${bin})

add_subdirectory(src/per-process)
add_subdirectory(src/per-open-hash)
add_subdirectory(src/per-open)
