// Deterministic delivered-signal checks are separate from the optional CPU-clock
// experiment. No wall-clock thresholds or natural timer counts in the CI suite.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <signal.h>
#endif
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
int luaopen_libplua(lua_State *);
int lrealstop(lua_State *);
}
#define CHECK(c) do { if (!(c)) { std::cerr << "CHECK failed: " #c << " line " << __LINE__ << '\n'; std::exit(1); } } while (0)
struct Sample { int count; std::vector<int> stack; };
struct Profile {
    std::map<int, std::string> names;
    std::vector<Sample> samples;
    int total = 0;
    int leaf(const std::string &name) const {
        int n = 0;
        for (const auto &s : samples)
            if (names.at(s.stack.back()).find(name) != std::string::npos) n += s.count;
        return n;
    }
};
static uint32_t u32(const std::vector<unsigned char> &d, size_t pos) {
    CHECK(pos+4 <= d.size());
    return (uint32_t)d[pos] | (uint32_t)d[pos+1]<<8 | (uint32_t)d[pos+2]<<16 | (uint32_t)d[pos+3]<<24;
}
static Profile read_profile(const char *path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), {});
    CHECK(d.size() >= 12 && u32(d, 0) == 0x41554C50);
    size_t header = u32(d, 4) == 2 ? 24 : 8;
    size_t end = d.size()-4;
    unsigned names = u32(d, end);
    Profile p;
    for (unsigned i=0; i<names; ++i) {
        end -= 4; int id = u32(d, end);
        end -= 4; unsigned len = u32(d, end);
        CHECK(end >= len+header); end -= len;
        p.names[id] = std::string((const char *)&d[end], len);
    }
    CHECK((end-header)%264 == 0);
    for (size_t pos=header; pos<end; pos+=264) {
        Sample s; s.count = u32(d, pos);
        unsigned depth = u32(d, pos+4);
        CHECK(depth>0 && depth<=64 && s.count>0);
        for (unsigned i=0; i<depth; ++i) {
            int id = u32(d, pos+8+i*4); CHECK(p.names.count(id));
            s.stack.push_back(id);
        }
        p.total += s.count; p.samples.push_back(s);
    }
    return p;
}
static void run(lua_State *L, const char *code, const char *name = "@precision.lua") {
    int rc = luaL_loadbuffer(L, code, std::strlen(code), name);
    if (rc == LUA_OK) rc = lua_pcall(L, 0, 0, 0);
    if (rc != LUA_OK) std::cerr << lua_tostring(L, -1) << '\n';
    CHECK(rc == LUA_OK);
}
static int emit(lua_State *L) {
#if !defined(_WIN32)
    int n = (int)luaL_checkinteger(L, 1);
    for (int i=0; i<n; ++i) CHECK(raise(SIGPROF) == 0);
#else
    (void)L;
#endif
    return 0;
}
static int bridge(lua_State *L) {
#if !defined(_WIN32)
    for (int i=0; i<11; ++i) raise(SIGPROF);
    lua_pushvalue(L, 1); lua_call(L, 0, 0);
    for (int i=0; i<13; ++i) raise(SIGPROF);
#endif
    return 0;
}
static int pending_stop(lua_State *L) {
#if !defined(_WIN32)
    for (int i=0; i<31; ++i) raise(SIGPROF);
    lrealstop(L); // no intervening Lua/count/return hook may drain these ticks
#endif
    return 0;
}
static int burn(lua_State *L) {
    double seconds = luaL_checknumber(L, 1);
    std::clock_t start = std::clock();
    volatile unsigned x = 1;
    while (double(std::clock()-start)/CLOCKS_PER_SEC < seconds)
        for (int i=0; i<10000; ++i) x = x*1664525u + 1013904223u;
    return 0;
}
static lua_State *state() {
    lua_State *L = luaL_newstate(); CHECK(L);
    luaL_openlibs(L);
    luaL_requiref(L, "libplua", luaopen_libplua, 1); lua_pop(L, 1);
    lua_pushcfunction(L, emit); lua_setglobal(L, "emit");
    lua_pushcfunction(L, bridge); lua_setglobal(L, "bridge");
    lua_pushcfunction(L, pending_stop); lua_setglobal(L, "pending_stop");
    lua_pushcfunction(L, burn); lua_setglobal(L, "burn");
    return L;
}
static void deterministic() {
#if !defined(_WIN32)
    lua_State *L = state();
    run(L,
        "p=require('libplua'); p.set_config{cpu_backend='sigprof',cpu_interval_ms=1000,cpu_hook_count=1000000}\n"
        "p.start(0,'precision_hot.pro')\n"
        "local function hot_caller() emit(37) end\n"
        "local function unrelated() local x=0; for i=1,20 do x=x+i end end\n"
        "hot_caller(); unrelated(); p.stop()\n");
    Profile p = read_profile("precision_hot.pro");
    CHECK(p.total == 37 && p.leaf("hot_caller") == 37 && p.leaf("unrelated") == 0);
    run(L,
        "p.start(0,'precision_bridge.pro')\n"
        "local function nested_caller()\n"
        " local function callback() emit(23) end\n"
        " bridge(callback)\n"
        "end\n"
        "nested_caller(); p.stop()\n");
    p = read_profile("precision_bridge.pro");
    CHECK(p.total == 47 && p.leaf("nested_caller") == 24 && p.leaf("precision.lua:3") == 23);
    run(L,
        "p.start(0,'precision_tail.pro')\n"
        "local function target() emit(17) end\n"
        "local function erased() return target() end\n"
        "erased(); p.stop()\n", "@precision_tail.lua");
    p = read_profile("precision_tail.pro");
    CHECK(p.total == 17 && p.leaf("precision_tail.lua:2") == 17);
    for (const auto &s : p.samples) for (int id : s.stack) CHECK(p.names[id].find("erased") == std::string::npos);
    run(L,
        "p.start(0,'precision_pending.pro')\n"
        "local function stop_caller() pending_stop() end\n"
        "stop_caller(); p.stop()\n");
    p = read_profile("precision_pending.pro");
    CHECK(p.total == 31 && p.leaf("stop_caller") == 31);
    run(L,
        "p.start(0,'precision_coroutine.pro')\n"
        "local co=coroutine.create(function() emit(19); coroutine.yield(); emit(29) end)\n"
        "local function parent()\n"
        " assert(coroutine.resume(co)); emit(7); assert(coroutine.resume(co)); emit(5)\n"
        "end\n"
        "parent(); p.stop()\n");
    p = read_profile("precision_coroutine.pro");
    CHECK(p.total == 12 && p.leaf("parent") == 12); // both unenrolled resumes excluded
    run(L,
        "p.start(0,'precision_parent.pro')\n"
        "local co=coroutine.create(function()\n"
        " p.start(0,'precision_child.pro')\n"
        " local function child() emit(41) end\n"
        " child(); p.stop()\n"
        "end)\n"
        "assert(coroutine.resume(co)); emit(3); p.stop()\n");
    CHECK(read_profile("precision_child.pro").leaf("child") == 41);
    CHECK(read_profile("precision_parent.pro").total == 3);
    run(L,
        "local preexisting=coroutine.create(function() emit(17) end)\n"
        "p.start(0,'precision_unhooked.pro')\n"
        "local function enclosing_resume() assert(coroutine.resume(preexisting)) end\n"
        "enclosing_resume(); p.stop()\n");
    // Public API cannot observe entry into a coroutine created before hook
    // installation: document this limit rather than claiming exact exclusion.
    CHECK(read_profile("precision_unhooked.pro").leaf("enclosing_resume") == 17);
    // Close a global state with an active coroutine session, then collect the
    // old anchor after a restart: neither may leak or destroy a new session.
    run(L,
        "local co=coroutine.create(function() p.start(0,'precision_close.pro'); emit(2); coroutine.yield() end)\n"
        "assert(coroutine.resume(co))\n");
    lua_close(L);
    CHECK(read_profile("precision_close.pro").total == 2);
    L = state();
    run(L, "p=require('libplua'); p.start(0,'precision_restart.pro'); p.stop(); p.start(0,'precision_restart.pro'); collectgarbage('collect'); emit(9); p.stop()");
    CHECK(read_profile("precision_restart.pro").total == 9);
    // Two independent states: only the active state gets a signal; idle host
    // ticks between complete chunks must not be charged to either state.
    lua_State *L2 = state();
    run(L, "p.start(0,'precision_state1.pro')");
    run(L2, "p=require('libplua'); p.start(0,'precision_state2.pro')");
    for (int i=0; i<15; ++i) raise(SIGPROF);
    run(L, "local function first() emit(11) end; first()");
    run(L2, "local function second() emit(22) end; second()");
    run(L, "p.stop()"); run(L2, "p.stop()");
    CHECK(read_profile("precision_state1.pro").total == 11);
    CHECK(read_profile("precision_state2.pro").total == 22);
    // A failed conflicting interval must neither steal ticks nor decrement the
    // first session's timer reference. File-start errors must be synchronous.
    run(L, "p.start(0,'precision_conflict.pro')");
    run(L2, "local rc,err=p.start(0,'precision_rejected.pro',500); assert(rc==-1 and type(err)=='string')");
    run(L, "local function still_running() emit(13) end; still_running(); p.stop()");
    CHECK(read_profile("precision_conflict.pro").leaf("still_running") == 13);
    run(L, "p.set_config{cpu_interval_ms=1000}; local rc,err=p.start(0,'missing-directory/no.pro'); assert(rc==-1 and type(err)=='string')");
    lua_close(L2);
    run(L, "p.start(5,'precision_limit.pro'); emit(9); p.stop()");
    CHECK(read_profile("precision_limit.pro").total == 5);
    lua_close(L);
    std::cout << "Deterministic: 37/37 C ticks to hot caller, unrelated=0; bridge 24+23/47; pending stop 31/31; tail/coroutine/lifecycle/multi-state/limit OK\n";
#endif
    // Count backend: no timer-dependent assertions, runs on Windows too.
    lua_State *C = state();
    run(C,
        "local p=require('libplua'); p.set_config{cpu_backend='count',cpu_hook_count=10}\n"
        "local function count_hot() local n=0; for i=1,10000 do n=n+i end end\n"
        "p.start(0,'precision_count.pro'); count_hot(); p.stop()\n");
    Profile pcount = read_profile("precision_count.pro");
    CHECK(pcount.total >= 1900 && pcount.total <= 2200);
    CHECK(pcount.leaf("count_hot") >= 1900);
    lua_close(C);
}
static void experiment() {
#if !defined(_WIN32)
    lua_State *L = state();
    run(L,
        "local p=require('libplua'); p.set_config{cpu_backend='sigprof',cpu_interval_ms=10,cpu_hook_count=1000}\n"
        "local function hot_caller() burn(0.6) end\n"
        "local function unrelated() local n=0; for i=1,1000 do n=n+i end end\n"
        "p.start(0,'precision_experiment.pro'); hot_caller(); unrelated(); p.stop()\n");
    Profile p = read_profile("precision_experiment.pro");
    std::cout << "EXPERIMENT (not a CI threshold): 600ms C CPU, nominal 60 ticks; total=" << p.total
              << " hot=" << p.leaf("hot_caller") << " unrelated=" << p.leaf("unrelated") << '\n';
    // Deliberately no statistical pass/fail threshold: run repeatedly and
    // inspect timer delivery bias; deterministic tests above gate correctness.
    lua_close(L);
#endif
}
int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--experiment") == 0) experiment();
    else deterministic();
}
