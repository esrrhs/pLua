#include <iostream>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#else
#include <io.h>
#endif

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

static void register_plua(lua_State *L) {
    luaL_requiref(L, "libplua", luaopen_libplua, 1);
    lua_pop(L, 1);
}

static int run_basic_tests() {
    std::cout << "[C++ Test] Initializing Lua state..." << std::endl;
    lua_State *L = luaL_newstate();
    ASSERT_TRUE(L != nullptr);
    luaL_openlibs(L);
    register_plua(L);

    std::cout << "[C++ Test] Testing CPU profiling..." << std::endl;
    const char *cpu_file = "cpp_test_cpu.pro";
    std::remove(cpu_file);

    int r = luaL_dostring(L,
        "local function fib(n) if n <= 1 then return n end return fib(n-1) + fib(n-2) end\n"
        "local p = require('libplua')\n"
        "p.start(0, 'cpp_test_cpu.pro')\n"
        "for i = 1, 50 do fib(22) end\n"
        "p.stop()\n"
    );
    if (r != 0) {
        std::cerr << lua_tostring(L, -1) << std::endl;
    }
    ASSERT_TRUE(r == 0);
    ASSERT_TRUE(file_exists_and_non_empty(cpu_file));
    std::remove(cpu_file);
    std::cout << "  -> CPU profiling OK" << std::endl;

    std::cout << "[C++ Test] Testing Memory profiling..." << std::endl;
    const char *mem_alloc = "cpp_test_mem_ALLOC_SIZE.pro";
    const char *mem_usage = "cpp_test_mem_USAGE.pro";
    std::remove(mem_alloc);
    std::remove(mem_usage);

    r = luaL_dostring(L,
        "local p = require('libplua')\n"
        "p.start_mem(0, 'cpp_test_mem.pro')\n"
        "local t = {}\n"
        "for i = 1, 50000 do t[i] = { id = i, s = tostring(i) } end\n"
        "collectgarbage('collect')\n"
        "p.stop_mem()\n"
    );
    if (r != 0) {
        std::cerr << lua_tostring(L, -1) << std::endl;
    }
    ASSERT_TRUE(r == 0);
    ASSERT_TRUE(file_exists_and_non_empty(mem_alloc));
    ASSERT_TRUE(file_exists_and_non_empty(mem_usage));
    std::remove(mem_alloc);
    std::remove(mem_usage);
    std::remove("plua.log");
    std::cout << "  -> Memory profiling OK" << std::endl;

    std::cout << "[C++ Test] Testing independent CPU + mem on same state..." << std::endl;
    std::remove(cpu_file);
    std::remove(mem_alloc);
    std::remove(mem_usage);
    r = luaL_dostring(L,
        "local function fib(n) if n <= 1 then return n end return fib(n-1) + fib(n-2) end\n"
        "local p = require('libplua')\n"
        "p.start(0, 'cpp_test_cpu.pro')\n"
        "p.start_mem(0, 'cpp_test_mem.pro')\n"
        "local t = {}\n"
        "for i = 1, 20000 do t[i] = { id = i } end\n"
        "for i = 1, 20 do fib(20) end\n"
        "collectgarbage('collect')\n"
        "p.stop()\n"
        "p.stop_mem()\n"
    );
    if (r != 0) {
        std::cerr << lua_tostring(L, -1) << std::endl;
    }
    ASSERT_TRUE(r == 0);
    ASSERT_TRUE(file_exists_and_non_empty(cpu_file));
    ASSERT_TRUE(file_exists_and_non_empty(mem_alloc));
    std::remove(cpu_file);
    std::remove(mem_alloc);
    std::remove(mem_usage);
    std::cout << "  -> Independent CPU+mem OK" << std::endl;

    lua_close(L);
    std::cout << "[C++ Test] All basic C++ unit tests passed!" << std::endl;
    return 0;
}

static void busy_work(lua_State *L) {
    int r = luaL_dostring(L,
        "local function fib(n) if n <= 1 then return n end return fib(n-1) + fib(n-2) end\n"
        "for i = 1, 30 do fib(20) end\n"
    );
    ASSERT_TRUE(r == 0);
}

static void mem_work(lua_State *L) {
    int r = luaL_dostring(L,
        "local t = {}\n"
        "for i = 1, 30000 do t[i] = { id = i, s = tostring(i) } end\n"
        "collectgarbage('collect')\n"
    );
    ASSERT_TRUE(r == 0);
}

static int run_multistate_tests() {
    std::cout << "[C++ MultiState] two states CPU..." << std::endl;
    lua_State *L1 = luaL_newstate();
    lua_State *L2 = luaL_newstate();
    ASSERT_TRUE(L1 && L2);
    luaL_openlibs(L1);
    luaL_openlibs(L2);
    register_plua(L1);
    register_plua(L2);

    const char *cpu1 = "ms_cpu1.pro";
    const char *cpu2 = "ms_cpu2.pro";
    std::remove(cpu1);
    std::remove(cpu2);

    ASSERT_TRUE(luaL_dostring(L1, "require('libplua').start(0, 'ms_cpu1.pro')") == 0);
    ASSERT_TRUE(luaL_dostring(L2, "require('libplua').start(0, 'ms_cpu2.pro')") == 0);
    busy_work(L1);
    busy_work(L2);
    ASSERT_TRUE(luaL_dostring(L1, "require('libplua').stop()") == 0);
    ASSERT_TRUE(luaL_dostring(L2, "require('libplua').stop()") == 0);
    ASSERT_TRUE(file_exists_and_non_empty(cpu1));
    ASSERT_TRUE(file_exists_and_non_empty(cpu2));
    std::remove(cpu1);
    std::remove(cpu2);
    std::cout << "  -> two states CPU OK" << std::endl;

    std::cout << "[C++ MultiState] two states mem..." << std::endl;
    const char *m1a = "ms_mem1_ALLOC_SIZE.pro";
    const char *m1u = "ms_mem1_USAGE.pro";
    const char *m2a = "ms_mem2_ALLOC_SIZE.pro";
    const char *m2u = "ms_mem2_USAGE.pro";
    std::remove(m1a); std::remove(m1u); std::remove(m2a); std::remove(m2u);
    ASSERT_TRUE(luaL_dostring(L1, "require('libplua').start_mem(0, 'ms_mem1.pro')") == 0);
    ASSERT_TRUE(luaL_dostring(L2, "require('libplua').start_mem(0, 'ms_mem2.pro')") == 0);
    mem_work(L1);
    mem_work(L2);
    ASSERT_TRUE(luaL_dostring(L1, "require('libplua').stop_mem()") == 0);
    ASSERT_TRUE(luaL_dostring(L2, "require('libplua').stop_mem()") == 0);
    ASSERT_TRUE(file_exists_and_non_empty(m1a));
    ASSERT_TRUE(file_exists_and_non_empty(m2a));
    std::remove(m1a); std::remove(m1u); std::remove(m2a); std::remove(m2u);
    std::cout << "  -> two states mem OK" << std::endl;

    std::cout << "[C++ MultiState] one CPU + one mem across states..." << std::endl;
    std::remove(cpu1);
    std::remove(m2a); std::remove(m2u);
    ASSERT_TRUE(luaL_dostring(L1, "require('libplua').start(0, 'ms_cpu1.pro')") == 0);
    ASSERT_TRUE(luaL_dostring(L2, "require('libplua').start_mem(0, 'ms_mem2.pro')") == 0);
    busy_work(L1);
    mem_work(L2);
    ASSERT_TRUE(luaL_dostring(L1, "require('libplua').stop()") == 0);
    ASSERT_TRUE(luaL_dostring(L2, "require('libplua').stop_mem()") == 0);
    ASSERT_TRUE(file_exists_and_non_empty(cpu1));
    ASSERT_TRUE(file_exists_and_non_empty(m2a));
    std::remove(cpu1); std::remove(m2a); std::remove(m2u);
    std::cout << "  -> cross-mode OK" << std::endl;

    std::cout << "[C++ MultiState] stop one leaves other..." << std::endl;
    std::remove(cpu1);
    std::remove(cpu2);
    ASSERT_TRUE(luaL_dostring(L1, "require('libplua').start(0, 'ms_cpu1.pro')") == 0);
    ASSERT_TRUE(luaL_dostring(L2, "require('libplua').start(0, 'ms_cpu2.pro')") == 0);
    busy_work(L1);
    busy_work(L2);
    ASSERT_TRUE(luaL_dostring(L1, "require('libplua').stop()") == 0);
    ASSERT_TRUE(file_exists_and_non_empty(cpu1));
    // L2 still running — more work then stop
    busy_work(L2);
    ASSERT_TRUE(luaL_dostring(L2, "require('libplua').stop()") == 0);
    ASSERT_TRUE(file_exists_and_non_empty(cpu2));
    std::remove(cpu1); std::remove(cpu2);
    std::cout << "  -> stop-one-leaves-other OK" << std::endl;

    lua_close(L1);
    lua_close(L2);
    std::remove("plua.log");
    std::cout << "[C++ MultiState] All multi-state tests passed!" << std::endl;
    return 0;
}

int main(int argc, char **argv) {
    bool multistate = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--multistate") == 0) multistate = true;
    }
    if (multistate) {
        return run_multistate_tests();
    }
    // Default: run both suites so a plain `unit_test` invocation is thorough
    int rc = run_basic_tests();
    if (rc != 0) return rc;
    return run_multistate_tests();
}
