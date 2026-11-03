#include <iostream>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <iterator>

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

// C function that lua_call's a Lua callback — leaves a C frame on the debug
// stack while the nested Lua code runs (and while pLua's count hook may fire).
static int c_invoke_lua_callback(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    int n = (int)luaL_optinteger(L, 2, 200);
    for (int i = 0; i < n; i++) {
        lua_pushvalue(L, 1);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            return lua_error(L);
        }
    }
    return 0;
}

static void register_c_callback_helper(lua_State *L) {
    lua_pushcfunction(L, c_invoke_lua_callback);
    lua_setglobal(L, "plua_c_invoke");
}

static bool file_contains(const std::string &path, const char *needle) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return data.find(needle) != std::string::npos;
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

    std::cout << "[C++ Test] Testing status() API..." << std::endl;
    r = luaL_dostring(L,
        "local p = require('libplua')\n"
        "local st = p.status()\n"
        "assert(type(st) == 'table')\n"
        "assert(st.cpu_running == false)\n"
        "assert(st.mem_running == false)\n"
        "assert(type(st.cpu_samples) == 'number')\n"
        "assert(type(st.cpu_backend) == 'string')\n"
        "assert(type(st.config) == 'table')\n"
        "p.start(0, 'cpp_test_cpu.pro')\n"
        "st = p.status()\n"
        "assert(st.cpu_running == true)\n"
        "assert(st.cpu_output == 'cpp_test_cpu.pro')\n"
        "for i = 1, 10 do end\n"
        "p.stop()\n"
        "st = p.status()\n"
        "assert(st.cpu_running == false)\n"
    );
    if (r != 0) {
        std::cerr << lua_tostring(L, -1) << std::endl;
    }
    ASSERT_TRUE(r == 0);
    std::remove(cpu_file);
    std::cout << "  -> status() OK" << std::endl;

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

    std::cout << "[C++ MultiState] close-after-stop / restart..." << std::endl;
    // Stop then close should not crash (registry __gc unregisters session).
    {
        lua_State *Lx = luaL_newstate();
        ASSERT_TRUE(Lx != nullptr);
        luaL_openlibs(Lx);
        register_plua(Lx);
        ASSERT_TRUE(luaL_dostring(Lx, "require('libplua').start(0, 'ms_close.pro')") == 0);
        busy_work(Lx);
        ASSERT_TRUE(luaL_dostring(Lx, "require('libplua').stop()") == 0);
        ASSERT_TRUE(file_exists_and_non_empty("ms_close.pro"));
        std::remove("ms_close.pro");
        lua_close(Lx); // exercises session_anchor __gc
    }
    // Start/stop/start again on a fresh state after previous close
    {
        lua_State *Ly = luaL_newstate();
        ASSERT_TRUE(Ly != nullptr);
        luaL_openlibs(Ly);
        register_plua(Ly);
        ASSERT_TRUE(luaL_dostring(Ly,
            "local p = require('libplua')\n"
            "assert(p.start(0, 'ms_restart.pro') == 0)\n"
            "local st = p.status()\n"
            "assert(st.cpu_running)\n"
            "p.stop()\n"
            "assert(p.status().cpu_running == false)\n"
            "assert(p.start(0, 'ms_restart.pro') == 0)\n"
            "p.stop()\n"
        ) == 0);
        ASSERT_TRUE(file_exists_and_non_empty("ms_restart.pro"));
        std::remove("ms_restart.pro");
        // Explicit close API
        ASSERT_TRUE(luaL_dostring(Ly, "require('libplua').close()") == 0);
        lua_close(Ly);
    }
    std::cout << "  -> close-after-stop / restart OK" << std::endl;

    lua_close(L1);
    lua_close(L2);
    std::remove("plua.log");
    std::cout << "[C++ MultiState] All multi-state tests passed!" << std::endl;
    return 0;
}

static int run_cattr_tests() {
    std::cout << "[C++ C-attr] Lua attribution through C callback..." << std::endl;
    lua_State *L = luaL_newstate();
    ASSERT_TRUE(L != nullptr);
    luaL_openlibs(L);
    register_plua(L);
    register_c_callback_helper(L);

    const char *cpu_file = "cpp_cattr_cpu.pro";
    const char *mem_alloc = "cpp_cattr_mem_ALLOC_SIZE.pro";
    const char *mem_usage = "cpp_cattr_mem_USAGE.pro";
    std::remove(cpu_file);
    std::remove(mem_alloc);
    std::remove(mem_usage);

    // Outer Lua frames call into C, which pcall's back into Lua. While the
    // nested Lua runs, the C frame sits on the debug stack; skipping it must
    // still attribute samples to these uniquely named Lua wrappers.
    int r = luaL_dostring(L,
        "local p = require('libplua')\n"
        "assert(p.set_config({ cpu_backend = 'count', cpu_hook_count = 20, mem_rate = 512 }) == 0)\n"
        "local function plua_cpp_cattr_cpu_outer()\n"
        "  local function leaf()\n"
        "    local s = string.rep('z', 64)\n"
        "    return string.find(s, 'zz', 1, true)\n"
        "  end\n"
        "  plua_c_invoke(leaf, 400)\n"
        "end\n"
        "assert(p.start(0, 'cpp_cattr_cpu.pro') == 0)\n"
        "plua_cpp_cattr_cpu_outer()\n"
        "assert(p.stop() == 0)\n"
        "local function plua_cpp_cattr_mem_outer()\n"
        "  local function leaf()\n"
        "    local t = {}\n"
        "    for i = 1, 200 do t[i] = string.rep(tostring(i), 16) end\n"
        "    return t\n"
        "  end\n"
        "  plua_c_invoke(leaf, 50)\n"
        "end\n"
        "assert(p.start_mem(0, 'cpp_cattr_mem.pro') == 0)\n"
        "plua_cpp_cattr_mem_outer()\n"
        "collectgarbage('collect')\n"
        "assert(p.stop_mem() == 0)\n"
    );
    if (r != 0) {
        std::cerr << lua_tostring(L, -1) << std::endl;
    }
    ASSERT_TRUE(r == 0);
    ASSERT_TRUE(file_exists_and_non_empty(cpu_file));
    ASSERT_TRUE(file_contains(cpu_file, "plua_cpp_cattr_cpu_outer"));
    ASSERT_TRUE(file_exists_and_non_empty(mem_alloc));
    // Mem samples taken while C wrapper is on the stack should still name Lua.
    // The nested alloc function is anonymous; accept either the leaf name from
    // CPU file already checked, or any non-empty Lua frame marker in mem file.
    ASSERT_TRUE(file_contains(mem_alloc, "plua_cpp_cattr_mem_outer"));

    std::remove(cpu_file);
    std::remove(mem_alloc);
    std::remove(mem_usage);
    std::remove("plua.log");
    lua_close(L);
    std::cout << "  -> C-callback Lua attribution OK" << std::endl;
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
    rc = run_cattr_tests();
    if (rc != 0) return rc;
    return run_multistate_tests();
}
