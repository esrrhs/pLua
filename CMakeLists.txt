cmake_minimum_required(VERSION 3.10)
project(pLua LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /DNDEBUG)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_BUILD_TYPE)
        add_compile_options(/O2)
    endif()
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -g3 -DNDEBUG -O3")
endif()

# Output directory for artifacts
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Find Lua dependency
find_package(Lua REQUIRED)

# Debian/Ubuntu often ship headers under include/lua5.x while also providing a
# dangling-ish /usr/include/lua.h symlink without sibling luaconf.h. Prefer the
# versioned include directory when present.
if(LUA_INCLUDE_DIR)
    if(NOT EXISTS "${LUA_INCLUDE_DIR}/luaconf.h")
        foreach(_ver 5.4 5.3 5.2)
            if(EXISTS "${LUA_INCLUDE_DIR}/lua${_ver}/luaconf.h")
                set(LUA_INCLUDE_DIR "${LUA_INCLUDE_DIR}/lua${_ver}")
                break()
            endif()
            if(EXISTS "/usr/include/lua${_ver}/luaconf.h")
                set(LUA_INCLUDE_DIR "/usr/include/lua${_ver}")
                break()
            endif()
        endforeach()
    endif()
endif()

# Look for lua CLI executable for testing
find_program(LUA_EXECUTABLE NAMES lua lua5.4 lua5.3 lua5.2 lua5.1)

add_subdirectory(src)

# Optionally build tools if Go is available
find_program(GO_EXECUTABLE go)
if(GO_EXECUTABLE)
    add_custom_target(tools_plua ALL
        COMMAND ${GO_EXECUTABLE} build -o ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plua${CMAKE_EXECUTABLE_SUFFIX} plua.go
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tools
        COMMENT "Building tools/plua via Go"
    )
endif()

# Testing
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()
