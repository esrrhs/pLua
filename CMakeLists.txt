cmake_minimum_required(VERSION 2.8)
project(plua)
aux_source_directory(. plua_src)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -g3 -std=c++11 -DNDEBUG")

add_library(plua SHARED ${plua_src})
TARGET_LINK_LIBRARIES(plua gvc)
