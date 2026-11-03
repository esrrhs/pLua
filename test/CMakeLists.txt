add_executable(unit_test unit_test.cpp)
target_include_directories(unit_test PRIVATE ${LUA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(unit_test PRIVATE plua ${LUA_LIBRARIES} ${CMAKE_DL_LIBS})

add_test(NAME CppUnitTest COMMAND unit_test)

if(LUA_EXECUTABLE)
    add_test(NAME LuaUnitTest COMMAND ${LUA_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_unit.lua
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Multi-state tests need the C++ harness (two lua_State* in one process)
add_test(NAME CppMultiStateTest COMMAND unit_test --multistate)
