#include <iostream>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
int luaopen_libplua(lua_State *L);
}

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            std::exit(1); \
        } \
    } while(0)

static bool file_exists_and_non_empty(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    return f.tellg() > 0;
}

int main() {
    std::cout << "[C++ Test] Initializing Lua state..." << std::endl;
    lua_State *L = luaL_newstate();
    ASSERT_TRUE(L != nullptr);
    luaL_openlibs(L);

    // Register plua module directly into package.loaded
    luaL_requiref(L, "libplua", luaopen_libplua, 1);
    lua_pop(L, 1);

    std::cout << "[C++ Test] Testing CPU profiling..." << std::endl;
    const char *cpu_file = "cpp_test_cpu.pro";
    std::remove(cpu_file);

    int r = luaL_dostring(L, "local p = require('libplua'); p.start(0, 'cpp_test_cpu.pro')");
    ASSERT_TRUE(r == 0);
    r = luaL_dostring(L, "local s = 0; for i = 1, 100000 do s = s + math.sin(i) end");
    ASSERT_TRUE(r == 0);
    r = luaL_dostring(L, "local p = require('libplua'); p.stop()");
    ASSERT_TRUE(r == 0);

    ASSERT_TRUE(file_exists_and_non_empty(cpu_file));
    std::remove(cpu_file);
    std::cout << "  -> CPU profiling OK" << std::endl;

    std::cout << "[C++ Test] Testing Memory profiling..." << std::endl;
    const char *mem_alloc = "cpp_test_mem_ALLOC_SIZE.pro";
    const char *mem_usage = "cpp_test_mem_USAGE.pro";
    std::remove(mem_alloc);
    std::remove(mem_usage);

    r = luaL_dostring(L, "local p = require('libplua'); p.start_mem(0, 'cpp_test_mem.pro')");
    ASSERT_TRUE(r == 0);
    r = luaL_dostring(L, "local t = {}; for i = 1, 50000 do t[i] = tostring(i) end; collectgarbage('collect')");
    ASSERT_TRUE(r == 0);
    r = luaL_dostring(L, "local p = require('libplua'); p.stop_mem()");
    ASSERT_TRUE(r == 0);

    ASSERT_TRUE(file_exists_and_non_empty(mem_alloc));
    ASSERT_TRUE(file_exists_and_non_empty(mem_usage));
    std::remove(mem_alloc);
    std::remove(mem_usage);
    std::remove("plua.log");
    std::cout << "  -> Memory profiling OK" << std::endl;

    lua_close(L);
    std::cout << "[C++ Test] All C++ unit tests passed!" << std::endl;
    return 0;
}
