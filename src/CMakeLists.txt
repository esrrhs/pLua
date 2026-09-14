add_library(plua SHARED plua.cpp)
target_include_directories(plua PRIVATE ${LUA_INCLUDE_DIR})
set_target_properties(plua PROPERTIES PREFIX "lib" OUTPUT_NAME "plua")

if(APPLE)
    set_target_properties(plua PROPERTIES LINK_FLAGS "-undefined dynamic_lookup")
endif()
