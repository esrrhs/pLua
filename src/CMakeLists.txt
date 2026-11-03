add_library(plua SHARED plua.cpp)
target_include_directories(plua PRIVATE ${LUA_INCLUDE_DIR})

# Produce libplua.so / libplua.dylib / libplua.dll so require("libplua") works
# consistently across platforms. On Windows this yields libplua.dll.
set_target_properties(plua PROPERTIES PREFIX "lib" OUTPUT_NAME "plua")

if(APPLE)
    set_target_properties(plua PROPERTIES
        LINK_FLAGS "-undefined dynamic_lookup"
        SUFFIX ".so"  # Lua on macOS commonly loads .so via package.cpath
    )
endif()

if(WIN32)
    # Export luaopen_* for require()
    set_target_properties(plua PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if(LUA_LIBRARIES)
    # Usually not required for a Lua C module (symbols come from the host),
    # but link when CMake provides them (helps some Windows toolchains).
    if(WIN32)
        target_link_libraries(plua PRIVATE ${LUA_LIBRARIES})
    endif()
endif()
