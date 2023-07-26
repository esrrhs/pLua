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
    int total;
};

ProfileData gProfileData;
static const int PROFILE_DATA_CALLSTACK_SIZE = 10240;

int gfd = 0;

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

        flush_file(gfd, (const char *) &count, sizeof(count));
        flush_file(gfd, (const char *) &cs, sizeof(cs));
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
        flush_file(gfd, str.c_str(), len);
        flush_file(gfd, (const char *) &len, sizeof(len));

        flush_file(gfd, (const char *) &id, sizeof(id));
        total_len++;
    }

    flush_file(gfd, (const char *) &total_len, sizeof(total_len));

    int total = gProfileData.total;
    LLOG("flush ok %d", gProfileData.total);

    gProfileData.total = 0;
    gProfileData.callstack.clear();

    if (gfd != 0) {
        close(gfd);
        gfd = 0;
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

static void SignalHandlerHook(lua_State *L, lua_Debug *par) {
    LLOG("Hook...");

    lua_sethook(gL, 0, 0, 0);

    if (gSampleCount != 0 && gSampleCount <= gProfileData.total) {
        LLOG("lrealstop...");
        lrealstop(L);
        return;
    }
    gProfileData.total++;

    lua_Debug ar;

    int last = lastlevel(L);
    int i = 0;

    CallStack cs;
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

    gProfileData.callstack[cs]++;
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

    gfd = fd;

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
    gProfileData.callstack.reserve(PROFILE_DATA_CALLSTACK_SIZE);

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

lua_Alloc gOldAlloc = NULL;
int gProfileRate = 0;
int gNextSample = 0;


static int gen_next_sample(int mean) {
    // TODO
    return 0;
}

static void *my_lua_Alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    LLOG("my_lua_Alloc %p %p %u %u", ud, ptr, osize, nsize);

    if (osize < nsize) {
        // alloc
        size_t alloc_sz = nsize - osize;

    }

    void *ret = gOldAlloc(ud, ptr, osize, nsize);
    return ret;
}

static int lrealstartmemsafe(lua_State *L) {
    int fd = open(gFilename.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        LERR("open file fail %s", gFilename.c_str());
        return -1;
    }

    gfd = fd;

    gProfileData.total = 0;
    gProfileData.callstack.clear();
    gProfileData.callstack.reserve(PROFILE_DATA_CALLSTACK_SIZE);

    // replace the realloc
    gOldAlloc = lua_getallocf(L, NULL);
    gNextSample = gen_next_sample(gSampleCount);
    lua_setallocf(L, my_lua_Alloc, NULL);

    return 0;
}

static void StartMemHandlerHook(lua_State *L, lua_Debug *par) {
    lua_sethook(L, 0, 0, 0);
    lrealstartmemsafe(L);
}

extern "C" int lrealstartmem(lua_State *L, int count, int profile_rate, const char *file) {
    if (gRunning) {
        LERR("start again, failed");
        return -1;
    }
    gRunning = 1;

    gL = L;
    gSampleCount = count;
    gProfileRate = profile_rate;
    gFilename = file;

    // lrealstartmem可能被注入调用，在StartMemHandlerHook里面执行具体的逻辑
    lua_sethook(gL, StartMemHandlerHook, LUA_MASKCOUNT, 1);

    LLOG("lstart %u %s", gSampleCount, file);

    return 0;
}

static int lstart_mem(lua_State *L) {
    int count = (int) lua_tointeger(L, 1);
    int profile_rate = (int) lua_tointeger(L, 2);
    const char *file = lua_tostring(L, 3);
    int ret = lrealstartmem(L, count, profile_rate, file);
    lua_pushinteger(L, ret);
    return 1;
}

static int lrealstopmemsafe(lua_State *L) {
    lua_sethook(L, 0, 0, 0);

    lua_setallocf(L, gOldAlloc, NULL);

    gRunning = 0;

    flush();

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
            {"stop",      lstop_mem},
            {NULL, NULL},
    };
    luaL_newlib(L, l);
    return 1;
}
