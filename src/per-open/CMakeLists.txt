# flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99 -Wall")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall")
add_definitions(-D_XOPEN_SOURCE=600 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE)

# Enable/Disabled gcc optimization
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")

# Enable/Disable address sanitizer
# add_compile_options( -fsanitize=address -static-libasan)

# Enable/Disabled debugging
#add_compile_options( -g)

include_directories(${IOLIB_INCLUDE_DIRS} ../shared ./)

add_library(griot-per-open SHARED ../shared/griot_tracer.c ../shared/hashmap.c ../shared/backtrace.c ../shared/log.c griot_model.c)
target_link_libraries(griot-per-open iolib iolog unwind)
target_compile_definitions(griot-per-open PRIVATE -DGRIOT_RANDOM_MACRO)

if (topbuild)
add_dependencies(fastio griot-per-open)
endif ()

install(TARGETS griot-per-open
	LIBRARY
	DESTINATION lib64)

