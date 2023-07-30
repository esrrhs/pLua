#include <string>
#include <list>
#include <vector>
#include <map>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <string>
#include <cstdlib>
#include <cstring>
#include <typeinfo>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <assert.h>
#include <math.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <unordered_map>
#include <fcntl.h>
#include <sstream>
#include <algorithm>
#include <vector>
#include <unordered_set>
#include <set>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

const int open_debug = 0;
int gSampleCount = 0;
std::string gFilename;
lua_State *gL = 0;
int gRunning = 0;

#define LLOG(...) if (open_debug) {llog("[DEBUG] ", __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__);}
#define LERR(...) if (open_debug) {llog("[ERROR] ", __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__);}

void llog(const char *header, const char *file, const char *func, int pos, const char *fmt, ...) {
    FILE *pLog = NULL;
    time_t clock1;
    struct tm *tptr;
    va_list ap;

    pLog = fopen("plua.log", "a+");
    if (pLog == NULL) {
        return;
    }

    clock1 = time(0);
    tptr = localtime(&clock1);

    struct timeval tv;
    gettimeofday(&tv, NULL);

    fprintf(pLog, "===========================[%d.%d.%d, %d.%d.%d %llu]%s:%d,%s:===========================\n%s",
            tptr->tm_year + 1990, tptr->tm_mon + 1,
            tptr->tm_mday, tptr->tm_hour, tptr->tm_min,
            tptr->tm_sec, (long long) ((tv.tv_sec) * 1000 + (tv.tv_usec) / 1000), file, pos, func, header);

    va_start(ap, fmt);
    vfprintf(pLog, fmt, ap);
    fprintf(pLog, "\n\n");
    va_end(ap);

    va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n\n");
    va_end(ap);

    fclose(pLog);
}

static const int MAX_FUNC_NAME_SIZE = 127;

/////////////////////////////copy from lua start///////////////////////////////////////////////

/*
** search for 'objidx' in table at index -1.
** return 1 + string at top if find a good name.
*/
static int findfield(lua_State *L, int objidx, int level) {
    if (level == 0 || !lua_istable(L, -1))
        return 0;  /* not found */
    lua_pushnil(L);  /* start 'next' loop */
    while (lua_next(L, -2)) {  /* for each pair in table */
        if (lua_type(L, -2) == LUA_TSTRING) {  /* ignore non-string keys */
            if (lua_rawequal(L, objidx, -1)) {  /* found object? */
                lua_pop(L, 1);  /* remove value (but keep name) */
                return 1;
            } else if (findfield(L, objidx, level - 1)) {  /* try recursively */
                lua_remove(L, -2);  /* remove table (but keep name) */
                lua_pushliteral(L, ".");
                lua_insert(L, -2);  /* place '.' between the two names */
                lua_concat(L, 3);
                return 1;
            }
        }
        lua_pop(L, 1);  /* remove value */
    }
    return 0;  /* not found */
}

/*
** Search for a name for a function in all loaded modules
*/
static int pushglobalfuncname(lua_State *L, lua_Debug *ar) {
    int top = lua_gettop(L);
    lua_getinfo(L, "f", ar);  /* push function */
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    if (findfield(L, top + 1, 2)) {
        const char *name = lua_tostring(L, -1);
        if (strncmp(name, "_G.", 3) == 0) {  /* name start with '_G.'? */
            lua_pushstring(L, name + 3);  /* push name without prefix */
            lua_remove(L, -2);  /* remove original name */
        }
        lua_copy(L, -1, top + 1);  /* move name to proper place */
        lua_pop(L, 2);  /* remove pushed values */
        return 1;
    } else {
        lua_settop(L, top);  /* remove function and global table */
        return 0;
    }
}


static void pushfuncname(lua_State *L, lua_Debug *ar) {
    if (pushglobalfuncname(L, ar)) {  /* try first a global name */
        lua_pushfstring(L, "function '%s'", lua_tostring(L, -1));
        lua_remove(L, -2);  /* remove name */
    } else if (*ar->namewhat != '\0')  /* is there a name from code? */
        lua_pushfstring(L, "%s '%s'", ar->namewhat, ar->name);  /* use it */
    else if (*ar->what == 'm')  /* main? */
        lua_pushliteral(L, "main chunk");
    else if (*ar->what != 'C')  /* for Lua functions, use <file:line> */
        lua_pushfstring(L, "function <%s:%d>", ar->short_src, ar->linedefined);
    else  /* nothing left... */
        lua_pushliteral(L, "?");
}


static int lastlevel(lua_State *L) {
    lua_Debug ar;
    int li = 1, le = 1;
    /* find the bottom index of the call stack. */
    while (lua_getstack(L, le, &ar)) {
        li = le;
        le *= 2;
    }
    /* do a binary search */
    while (li < le) {
        int m = (li + le) / 2;
        if (lua_getstack(L, m, &ar)) li = m + 1;
        else le = m;
    }
    return le - 1;
}


/////////////////////////////copy from lua end///////////////////////////////////////////////

std::unordered_map<std::string, int> gString2Id;
std::unordered_map<int, std::string> gId2String;

static const char *IGNORE_NAME[] = {"?", "function 'xpcall'", "function 'pcall'", "function", "function 'tcall'",
                                    "function 'txpcall'", "local 'func'"};
static const int VALID_MIN_ID = sizeof(IGNORE_NAME) / sizeof(const char *);

static const int MAX_STACK_SIZE = 64;

static const int CPU_SAMPLE_ITER = 10;

struct CallStack {
    int depth;
    int stack[MAX_STACK_SIZE];
};

struct CallStackHash {
    size_t operator()(const CallStack &cs) const {
        size_t hash = 0;
        for (int i = 0; i < cs.depth; i++) {
            int id = cs.stack[i];
            hash = (hash << 8) | (hash >> (8 * (sizeof(hash) - 1)));
            hash += (id * 31) + (id * 7) + (id * 3);
        }
        return hash;
    }
};

struct CallStackEqual {
    bool operator()(const CallStack &cs1, const CallStack &cs2) const {
        if (cs1.depth != cs2.depth) {
            return false;
        }
        return memcmp(cs1.stack, cs2.stack, sizeof(int) * cs1.depth) == 0;
    }
};

struct ProfileData {
    std::unordered_map<CallStack, int, CallStackHash, CallStackEqual> callstack;
    int total = 0;
    int fd = 0;
};

ProfileData gProfileData;

static void flush_file(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t r = write(fd, buf, len);
        buf += r;
        len -= r;
    }
}

static void flush() {
    if (gProfileData.total <= 0) {
        return;
    }

    LLOG("flush...");

    for (auto iter = gProfileData.callstack.begin(); iter != gProfileData.callstack.end(); iter++) {
        const CallStack &cs = iter->first;
        int count = iter->second;

        flush_file(gProfileData.fd, (const char *) &count, sizeof(count));
        flush_file(gProfileData.fd, (const char *) &cs, sizeof(cs));
    }

    int total_len = 0;
    for (auto iter = gString2Id.begin(); iter != gString2Id.end(); iter++) {
        const std::string &str = iter->first;
        int id = iter->second;

        if (id < VALID_MIN_ID) {
            continue;
        }

        int len = str.length();
        len = len > MAX_FUNC_NAME_SIZE ? MAX_FUNC_NAME_SIZE : len;
        flush_file(gProfileData.fd, str.c_str(), len);
        flush_file(gProfileData.fd, (const char *) &len, sizeof(len));

        flush_file(gProfileData.fd, (const char *) &id, sizeof(id));
        total_len++;
    }

    flush_file(gProfileData.fd, (const char *) &total_len, sizeof(total_len));

    int total = gProfileData.total;
    LLOG("flush ok %d", gProfileData.total);

    gProfileData.total = 0;
    gProfileData.callstack.clear();

    if (gProfileData.fd != 0) {
        close(gProfileData.fd);
        gProfileData.fd = 0;
    }

    printf("pLua flush ok %d\n", total);
}

static int lrealstopsafe(lua_State *L) {
    lua_sethook(L, 0, 0, 0);

    gRunning = 0;

    struct itimerval timer;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0;
    timer.it_value = timer.it_interval;
    int ret = setitimer(ITIMER_PROF, &timer, NULL);
    if (ret != 0) {
        LERR("setitimer fail %d", ret);
        return ret;
    }

    flush();

    return 0;
}

static void StopHandlerHook(lua_State *L, lua_Debug *par) {
    lua_sethook(L, 0, 0, 0);
    lrealstopsafe(L);
}

extern "C" int lrealstop(lua_State *L) {
    // lrealstop可能被注入调用，在StopHandlerHook里面执行具体的逻辑
    lua_sethook(gL, StopHandlerHook, LUA_MASKCOUNT, 1);
    return 0;
}

static void get_cur_callstack(lua_State *L, CallStack &cs) {
    lua_Debug ar;

    int last = lastlevel(L);
    int i = 0;

    cs.depth = 0;

    while (lua_getstack(L, last, &ar) && i < MAX_STACK_SIZE) {
        lua_getinfo(L, "Slnt", &ar);
        pushfuncname(L, &ar);
        const char *funcname = lua_tostring(L, -1);
        lua_pop(L, 1);

        i++;
        last--;

        int id = 0;
        auto iter = gString2Id.find(funcname);
        if (iter == gString2Id.end()) {
            id = gString2Id.size();
            gString2Id[funcname] = id;
            gId2String[id] = funcname;
        } else {
            id = iter->second;
        }

        if (id < VALID_MIN_ID) {
            continue;
        }

        LLOG("%s %d %d", funcname, id, last);

        cs.stack[cs.depth] = id;
        cs.depth++;
    }
}

static void SignalHandlerHook(lua_State *L, lua_Debug *par) {
    LLOG("Hook...");

    lua_sethook(gL, 0, 0, 0);

    if (gSampleCount != 0 && gSampleCount <= gProfileData.total) {
        LLOG("lrealstop...");
        lrealstop(L);
        return;
    }

    CallStack cs;
    get_cur_callstack(L, cs);

    gProfileData.callstack[cs]++;
    gProfileData.total++;
}

static void SignalHandler(int sig, siginfo_t *sinfo, void *ucontext) {
    lua_sethook(gL, SignalHandlerHook, LUA_MASKCOUNT, 1);
}

static int lrealstartsafe(lua_State *L) {
    if (gRunning) {
        LERR("start again, failed");
        return -1;
    }
    gRunning = 1;

    for (int i = 0; i < VALID_MIN_ID; i++) {
        gString2Id[IGNORE_NAME[i]] = i;
        gId2String[i] = IGNORE_NAME[i];
    }

    struct sigaction sa;
    sa.sa_sigaction = SignalHandler;
    sa.sa_flags = SA_RESTART | SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGPROF, &sa, NULL) == -1) {
        LERR("sigaction(SIGALRM) failed");
        return -1;
    }

    int fd = open(gFilename.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        LERR("open file fail %s", gFilename.c_str());
        return -1;
    }

    gProfileData.fd = fd;

    struct itimerval timer;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = CPU_SAMPLE_ITER * 1000;
    timer.it_value = timer.it_interval;
    int ret = setitimer(ITIMER_PROF, &timer, NULL);
    if (ret != 0) {
        LERR("setitimer fail %d", ret);
        return -1;
    }

    gProfileData.total = 0;
    gProfileData.callstack.clear();

    return 0;
}

static void StartHandlerHook(lua_State *L, lua_Debug *par) {
    lua_sethook(L, 0, 0, 0);
    lrealstartsafe(L);
}

extern "C" int lrealstart(lua_State *L, int second, const char *file) {
    if (gRunning) {
        LERR("start again, failed");
        return -1;
    }

    gL = L;
    gSampleCount = second * 1000 / CPU_SAMPLE_ITER;
    gFilename = file;

    // lrealstart可能被注入调用，在StartHandlerHook里面执行具体的逻辑
    lua_sethook(gL, StartHandlerHook, LUA_MASKCOUNT, 1);

    LLOG("lstart %u %s", gSampleCount, file);

    return 0;
}

static int lstart(lua_State *L) {
    int second = (int) lua_tointeger(L, 1);
    const char *file = lua_tostring(L, 2);
    int ret = lrealstart(L, second, file);
    lua_pushinteger(L, ret);
    return 1;
}

static int lstop(lua_State *L) {
    LLOG("lstop %s", gFilename.c_str());
    int ret = lrealstop(L);
    lua_pushinteger(L, ret);
    return 1;
}

//////////////////////////////////mem profiler start////////////////////////////////////////

static const int MEM_PROFILE_RATE = 524288;  // 512K

struct MemInfo {
    MemInfo() {}

    MemInfo(uint32_t a, uint32_t as, uint32_t f, uint32_t fs) {
        allocs = a;
        alloc_size = as;
        frees = f;
        free_size = fs;
    }

    uint32_t allocs = 0;
    uint32_t alloc_size = 0;
    uint32_t frees = 0;
    uint32_t free_size = 0;
};

struct CallStackPointerHash {
    size_t operator()(CallStack *cs) const {
        size_t hash = 0;
        for (int i = 0; i < cs->depth; i++) {
            int id = cs->stack[i];
            hash = (hash << 8) | (hash >> (8 * (sizeof(hash) - 1)));
            hash += (id * 31) + (id * 7) + (id * 3);
        }
        return hash;
    }
};

struct CallStackPointerEqual {
    bool operator()(CallStack *cs1, CallStack *cs2) const {
        if (cs1->depth != cs2->depth) {
            return false;
        }
        return memcmp(cs1->stack, cs2->stack, sizeof(int) * cs1->depth) == 0;
    }
};

struct MemProfileData {
    std::unordered_map<CallStack *, MemInfo, CallStackPointerHash, CallStackPointerEqual> callstack;
    std::unordered_map<void *, CallStack *> ptr2Callstack;
    int total = 0;
    lua_Alloc oldAlloc = NULL;
    size_t nextSample = 0;
    bool isInAlloc = false;
    uint64_t rand = 0;
    int allocs = 0;
    int64_t alloc_size = 0;
    int frees = 0;
    int64_t free_size = 0;
    int alloc_size_fd = 0;
    int alloc_count_fd = 0;
    int usage_fd = 0;
};

MemProfileData gMemProfileData;

// 移植自gperftools的Sampler::NextRandom
static uint64_t next_random(uint64_t rnd) {
    const uint64_t prng_mult = 0x5DEECE66DULL;
    const uint64_t prng_add = 0xB;
    const uint64_t prng_mod_power = 48;
    const uint64_t prng_mod_mask = ~((~static_cast<uint64_t>(0)) << prng_mod_power);
    return (prng_mult * rnd + prng_add) & prng_mod_mask;
}

#define MAX_SSIZE (static_cast<ssize_t>(static_cast<size_t>(static_cast<ssize_t>(-1)) >> 1))

// 移植自gperftools的Sampler::PickNextSamplingPoint
static size_t gen_next_sample() {
    gMemProfileData.rand = next_random(gMemProfileData.rand);
    // Take the top 26 bits as the random number
    // (This plus the 1<<58 sampling bound give a max possible step of
    // 5194297183973780480 bytes.)
    const uint64_t prng_mod_power = 48;  // Number of bits in prng
    // The uint32_t cast is to prevent a (hard-to-reproduce) NAN
    // under piii debug for some binaries.
    double q = static_cast<uint32_t>(gMemProfileData.rand >> (prng_mod_power - 26)) + 1.0;
    // Put the computed p-value through the CDF of a geometric.
    double interval = (log2(q) - 26) * (-log(2.0) * MEM_PROFILE_RATE);

    // Very large values of interval overflow ssize_t. If we happen to
    // hit such improbable condition, we simply cheat and clamp interval
    // to largest supported value.
    return static_cast<size_t>(std::min<double>(interval, static_cast<double>(MAX_SSIZE)));
}

static void flush_mem_left(int fd) {
    int total_len = 0;
    for (auto iter = gString2Id.begin(); iter != gString2Id.end(); iter++) {
        const std::string &str = iter->first;
        int id = iter->second;

        if (id < VALID_MIN_ID) {
            continue;
        }

        int len = str.length();
        len = len > MAX_FUNC_NAME_SIZE ? MAX_FUNC_NAME_SIZE : len;
        flush_file(fd, str.c_str(), len);
        flush_file(fd, (const char *) &len, sizeof(len));

        flush_file(fd, (const char *) &id, sizeof(id));
        total_len++;
    }

    flush_file(fd, (const char *) &total_len, sizeof(total_len));

    if (fd != 0) {
        close(fd);
    }
}

static void flush_mem_alloc_size() {
    int fd = gMemProfileData.alloc_size_fd;

    for (auto iter = gMemProfileData.callstack.begin(); iter != gMemProfileData.callstack.end(); iter++) {
        const CallStack &cs = *iter->first;
        auto &mem_info = iter->second;

        flush_file(fd, (const char *) &mem_info.alloc_size, sizeof(mem_info.alloc_size));
        flush_file(fd, (const char *) &cs, sizeof(cs));
    }

    flush_mem_left(fd);
    gMemProfileData.alloc_size_fd = 0;
}

static void flush_mem_alloc_count() {
    int fd = gMemProfileData.alloc_count_fd;

    for (auto iter = gMemProfileData.callstack.begin(); iter != gMemProfileData.callstack.end(); iter++) {
        const CallStack &cs = *iter->first;
        auto &mem_info = iter->second;

        flush_file(fd, (const char *) &mem_info.allocs, sizeof(mem_info.allocs));
        flush_file(fd, (const char *) &cs, sizeof(cs));
    }

    flush_mem_left(fd);
    gMemProfileData.alloc_count_fd = 0;
}

static void flush_mem_usage() {
    int fd = gMemProfileData.usage_fd;

    for (auto iter = gMemProfileData.callstack.begin(); iter != gMemProfileData.callstack.end(); iter++) {
        const CallStack &cs = *iter->first;
        auto &mem_info = iter->second;
        auto usage = (int) mem_info.alloc_size - (int) mem_info.free_size;
        if (usage <= 0) {
            continue;
        }

        flush_file(fd, (const char *) &usage, sizeof(usage));
        flush_file(fd, (const char *) &cs, sizeof(cs));
    }

    flush_mem_left(fd);
    gMemProfileData.usage_fd = 0;
}

static void flush_mem() {
    if (gMemProfileData.total <= 0) {
        return;
    }

    LLOG("flush...");

    flush_mem_alloc_size();
    flush_mem_alloc_count();
    flush_mem_usage();

    int total = gMemProfileData.total;
    LLOG("flush ok %d", gMemProfileData.total);

    gMemProfileData.total = 0;
    for (auto iter = gMemProfileData.callstack.begin(); iter != gMemProfileData.callstack.end(); iter++) {
        delete iter->first;
    }
    gMemProfileData.callstack.clear();
    gMemProfileData.ptr2Callstack.clear();

    printf("pLua flush ok %d\n", total);
}

static int lrealstopmemsafe(lua_State *L) {
    lua_sethook(L, 0, 0, 0);

    lua_setallocf(L, gMemProfileData.oldAlloc, NULL);

    gRunning = 0;

    flush_mem();

    return 0;
}

static void StopMemHandlerHook(lua_State *L, lua_Debug *par) {
    lua_sethook(L, 0, 0, 0);
    lrealstopmemsafe(L);
}

extern "C" int lrealstopmem(lua_State *L) {
    // lrealstopmem可能被注入调用，在StopMemHandlerHook里面执行具体的逻辑
    lua_sethook(gL, StopMemHandlerHook, LUA_MASKCOUNT, 1);
    return 0;
}

static void *my_lua_Alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    //LLOG("my_lua_Alloc %p %p %u %u", ud, ptr, osize, nsize);

    // 防止重入，get_cur_callstack是可能触发lua内存分配的
    if (gMemProfileData.isInAlloc) {
        return gMemProfileData.oldAlloc(ud, ptr, osize, nsize);
    }
    gMemProfileData.isInAlloc = true;

    // check stop if set sample count
    if (gSampleCount != 0 && gSampleCount <= gMemProfileData.total) {
        LLOG("lrealstopmem...");
        lrealstopmem(gL);
        gMemProfileData.isInAlloc = false;
        return gMemProfileData.oldAlloc(ud, ptr, osize, nsize);
    }

    if (osize < nsize) {

        // is alloc
        size_t alloc_sz = nsize - osize;

        if (osize > 0 && ptr) {
            // remove old pointer
            gMemProfileData.ptr2Callstack.erase(ptr);
        }

        if (alloc_sz < gMemProfileData.nextSample) {
            gMemProfileData.nextSample -= alloc_sz;
            gMemProfileData.isInAlloc = false;
            return gMemProfileData.oldAlloc(ud, ptr, osize, nsize);
        }

        gMemProfileData.nextSample = gen_next_sample();

        // start profile
        gMemProfileData.total++;

        CallStack cs;
        get_cur_callstack(gL, cs);

        auto it = gMemProfileData.callstack.find(&cs);
        CallStack *pointer_cs = 0;
        if (it == gMemProfileData.callstack.end()) {
            auto new_cs = new CallStack();
            memcpy(new_cs, &cs, sizeof(cs));
            gMemProfileData.callstack[new_cs] = MemInfo(1, alloc_sz, 0, 0);
            pointer_cs = new_cs;
        } else {
            auto &mem_info = it->second;
            mem_info.allocs++;
            mem_info.alloc_size += alloc_sz;
            pointer_cs = it->first;
        }

        gMemProfileData.allocs++;
        gMemProfileData.alloc_size += alloc_sz;

        void *ret = gMemProfileData.oldAlloc(ud, ptr, osize, nsize);

        // add new pointer
        gMemProfileData.ptr2Callstack[ret] = pointer_cs;

        LLOG("alloc %p %u %u %u %u", ret, osize, nsize, gMemProfileData.allocs, gMemProfileData.alloc_size);

        gMemProfileData.isInAlloc = false;
        return ret;

    } else if (osize > nsize) {

        // free some memory
        size_t free_sz = osize - nsize;

        // remove old pointer
        auto it = gMemProfileData.ptr2Callstack.find(ptr);
        if (it == gMemProfileData.ptr2Callstack.end()) {
            gMemProfileData.isInAlloc = false;
            return gMemProfileData.oldAlloc(ud, ptr, osize, nsize);
        }

        CallStack *cs = it->second;
        gMemProfileData.ptr2Callstack.erase(it);

        auto it2 = gMemProfileData.callstack.find(cs);
        if (it2 != gMemProfileData.callstack.end()) {
            auto &mem_info = it2->second;
            mem_info.frees++;
            mem_info.free_size += free_sz;

            gMemProfileData.frees++;
            gMemProfileData.free_size += free_sz;

            LLOG("free %p %u %u %u %u", ptr, osize, nsize, gMemProfileData.frees, gMemProfileData.free_size);
        }

        void *ret = gMemProfileData.oldAlloc(ud, ptr, osize, nsize);
        if (nsize > 0 && ret) {
            // add new pointer
            gMemProfileData.ptr2Callstack[ret] = cs;
        }

        gMemProfileData.isInAlloc = false;
        return ret;
    } else {
        gMemProfileData.isInAlloc = false;
        return gMemProfileData.oldAlloc(ud, ptr, osize, nsize);
    }
}

static int lrealstartmemsafe(lua_State *L) {
    if (gRunning) {
        LERR("start again, failed");
        return -1;
    }
    gRunning = 1;

    for (int i = 0; i < VALID_MIN_ID; i++) {
        gString2Id[IGNORE_NAME[i]] = i;
        gId2String[i] = IGNORE_NAME[i];
    }

    int fd = open((std::string("ALLOC_SIZE_") + gFilename).c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        LERR("open file fail %s", gFilename.c_str());
        return -1;
    }
    gMemProfileData.alloc_size_fd = fd;

    fd = open((std::string("ALLOC_COUNT_") + gFilename).c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        LERR("open file fail %s", gFilename.c_str());
        return -1;
    }
    gMemProfileData.alloc_count_fd = fd;

    fd = open((std::string("USAGE_") + gFilename).c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        LERR("open file fail %s", gFilename.c_str());
        return -1;
    }
    gMemProfileData.usage_fd = fd;

    gMemProfileData.total = 0;
    for (auto iter = gMemProfileData.callstack.begin(); iter != gMemProfileData.callstack.end(); iter++) {
        delete iter->first;
    }
    gMemProfileData.callstack.clear();
    gMemProfileData.ptr2Callstack.clear();

    // replace the realloc
    gMemProfileData.oldAlloc = lua_getallocf(L, NULL);
    lua_setallocf(L, my_lua_Alloc, NULL);
    gMemProfileData.rand = time(NULL);
    // Step it forward 20 times for good measure
    for (int i = 0; i < 20; i++) {
        gMemProfileData.rand = next_random(gMemProfileData.rand);
    }
    gMemProfileData.nextSample = gen_next_sample();
    gMemProfileData.isInAlloc = false;
    gMemProfileData.allocs = 0;
    gMemProfileData.alloc_size = 0;
    gMemProfileData.frees = 0;
    gMemProfileData.free_size = 0;

    return 0;
}

static void StartMemHandlerHook(lua_State *L, lua_Debug *par) {
    lua_sethook(L, 0, 0, 0);
    lrealstartmemsafe(L);
}

extern "C" int lrealstartmem(lua_State *L, int count, const char *file) {
    if (gRunning) {
        LERR("start again, failed");
        return -1;
    }

    gL = L;
    gSampleCount = count;
    gFilename = file;
    gMemProfileData.oldAlloc = NULL;
    gMemProfileData.nextSample = 0;
    gMemProfileData.isInAlloc = false;

    // lrealstartmem可能被注入调用，在StartMemHandlerHook里面执行具体的逻辑
    lua_sethook(gL, StartMemHandlerHook, LUA_MASKCOUNT, 1);

    LLOG("lstart %u %s", gSampleCount, file);

    return 0;
}

static int lstart_mem(lua_State *L) {
    int count = (int) lua_tointeger(L, 1);
    const char *file = lua_tostring(L, 2);
    int ret = lrealstartmem(L, count, file);
    lua_pushinteger(L, ret);
    return 1;
}

static int lstop_mem(lua_State *L) {
    LLOG("lstop %s", gFilename.c_str());
    int ret = lrealstopmem(L);
    lua_pushinteger(L, ret);
    return 1;
}

//////////////////////////////////mem profiler end//////////////////////////////////////////

extern "C" int luaopen_libplua(lua_State *L) {
    luaL_checkversion(L);
    luaL_Reg l[] = {
            // for cpu
            {"start",     lstart},
            {"stop",      lstop},

            // for memory
            {"start_mem", lstart_mem},
            {"stop_mem",  lstop_mem},
            {NULL, NULL},
    };
    luaL_newlib(L, l);
    return 1;
}
