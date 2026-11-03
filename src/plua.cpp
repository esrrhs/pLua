#include <string>
#include <vector>
#include <map>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <time.h>
#include <stdarg.h>
#include <assert.h>
#include <math.h>
#include <errno.h>
#include <unordered_map>
#include <sstream>
#include <algorithm>
#include <unordered_set>
#include <set>
#include <atomic>

#if defined(_WIN32)
#  define PLUA_WINDOWS 1
#  ifndef _SSIZE_T_DEFINED
#    include <BaseTsd.h>
     typedef SSIZE_T ssize_t;
#    define _SSIZE_T_DEFINED
#  endif
#  include <io.h>
#  include <fcntl.h>
#  include <sys/types.h>
#  include <sys/stat.h>
#  define open _open
#  define close _close
#  define write _write
#  define O_CREAT _O_CREAT
#  define O_WRONLY _O_WRONLY
#  define O_TRUNC _O_TRUNC
#  ifndef S_IRUSR
#    define S_IRUSR _S_IREAD
#    define S_IWUSR _S_IWRITE
#  endif
#else
#  include <sys/time.h>
#  include <signal.h>
#  include <unistd.h>
#  include <fcntl.h>
#  define PLUA_HAS_POSIX_PROF 1
#endif

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#if LUA_VERSION_NUM < 502
#  error "pLua requires Lua 5.2 or newer (Lua 5.1 / LuaJIT are not supported by this build)"
#endif

const int open_debug = 0;

#define LLOG(...) if (open_debug) {llog("[DEBUG] ", __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__);}
#define LERR(...) if (open_debug) {llog("[ERROR] ", __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__);}

static void llog(const char *header, const char *file, const char *func, int pos, const char *fmt, ...) {
    FILE *pLog = fopen("plua.log", "a+");
    if (pLog == NULL) {
        return;
    }

    time_t clock1 = time(0);
    struct tm *tptr = localtime(&clock1);

    long long ms = 0;
#if defined(PLUA_WINDOWS)
    ms = (long long)clock1 * 1000;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ms = (long long)((tv.tv_sec) * 1000 + (tv.tv_usec) / 1000);
#endif

    fprintf(pLog, "===========================[%d.%d.%d, %d.%d.%d %lld]%s:%d,%s:===========================\n%s",
            tptr->tm_year + 1900, tptr->tm_mon + 1,
            tptr->tm_mday, tptr->tm_hour, tptr->tm_min,
            tptr->tm_sec, ms, file, pos, func, header);

    va_list ap;
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
static const int MAX_STACK_SIZE = 64;
static const int DEFAULT_CPU_SAMPLE_ITER_MS = 10;
static const int DEFAULT_CPU_HOOK_COUNT = 1000;
static const int DEFAULT_MEM_PROFILE_RATE = 524288;  // 512K
static const uint32_t PLUA_PRO_MAGIC = 0x41554C50; // 'PLUA' LE
static const uint32_t PLUA_PRO_VERSION = 1;

// Build a stable display name for a Lua debug frame.
// C frames are normally skipped in get_cur_callstack (vLua-style); this helper
// still formats them if a caller needs a label.
static std::string get_funcname(lua_State *L, lua_Debug *ar) {
    (void)L;
    char buf[MAX_FUNC_NAME_SIZE + 1] = {0};
    if (ar->namewhat != NULL && *ar->namewhat != '\0' && ar->name != NULL) {
        snprintf(buf, MAX_FUNC_NAME_SIZE, "%s '%s'", ar->namewhat, ar->name);
    } else if (ar->what != NULL && *ar->what == 'm') {
        snprintf(buf, MAX_FUNC_NAME_SIZE, "main chunk");
    } else if (ar->what == NULL || *ar->what != 'C') {
        snprintf(buf, MAX_FUNC_NAME_SIZE, "function <%s:%d>",
                 ar->short_src, ar->linedefined);
    } else {
        // Rare: C frame label (usually filtered out when walking the stack)
        snprintf(buf, MAX_FUNC_NAME_SIZE, "[C] %s", ar->name ? ar->name : "?");
    }
    return buf;
}

static int lastlevel(lua_State *L) {
    lua_Debug ar;
    int li = 1, le = 1;
    while (lua_getstack(L, le, &ar)) {
        li = le;
        le *= 2;
    }
    while (li < le) {
        int m = (li + le) / 2;
        if (lua_getstack(L, m, &ar)) li = m + 1;
        else le = m;
    }
    return le - 1;
}

static const char *IGNORE_NAME[] = {"?", "function 'xpcall'", "upvalue 'xpcall'", "field 'xpcall'",
                                    "function 'pcall'", "upvalue 'pcall'", "field 'pcall'",
                                    "function", "local 'func'"};
static const int VALID_MIN_ID = sizeof(IGNORE_NAME) / sizeof(const char *);

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
    int fd = -1;
};

struct MemInfo {
    MemInfo() {}
    MemInfo(uint32_t a, uint32_t f) : allocs(a), frees(f) {}
    uint32_t allocs = 0;
    uint32_t frees = 0;
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
    void *oldAllocUd = NULL;
    ssize_t nextSample = 0;
    uint64_t rand = 0;
    int alloc_size_fd = -1;
    int usage_fd = -1;
    int is_in_hook = 0;
    void *hook_alloc_ptr = 0;
    size_t hook_alloc_sz = 0;
};

struct ProfilerSession {
    lua_State *L = NULL;
    bool cpuRunning = false;
    bool memRunning = false;
    bool useSigprof = false;

    int cpuSampleLimit = 0;       // 0 = unlimited (from duration seconds)
    int cpuIntervalMs = DEFAULT_CPU_SAMPLE_ITER_MS;
    int cpuHookCount = DEFAULT_CPU_HOOK_COUNT;
    std::string cpuFilename;
    ProfileData cpuData;

    int memSampleLimit = 0;
    int memProfileRate = DEFAULT_MEM_PROFILE_RATE;
    std::string memFilename;
    MemProfileData memData;

    std::unordered_map<std::string, int> string2Id;
    std::unordered_map<int, std::string> id2String;

    // Atomically set from SIGPROF; drained only inside SessionHook (never lua_sethook in signal).
    std::atomic<bool> pendingCpuSample{false};
    std::atomic<bool> pendingMemSample{false};
};

static std::map<lua_State *, ProfilerSession *> gSessions;
#if defined(PLUA_HAS_POSIX_PROF)
static int gPosixTimerUsers = 0;
static bool gSigHandlerInstalled = false;
#endif

// Global defaults (overridable via env / Lua API before start)
static int gDefaultCpuIntervalMs = DEFAULT_CPU_SAMPLE_ITER_MS;
static int gDefaultCpuHookCount = DEFAULT_CPU_HOOK_COUNT;
static int gDefaultMemProfileRate = DEFAULT_MEM_PROFILE_RATE;
static int gForceCountBackend = 0; // 1 = force LUA_MASKCOUNT

static int env_int(const char *name, int defval) {
    const char *v = getenv(name);
    if (!v || !*v) return defval;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) return defval;
    return (int)n;
}

static void load_env_defaults() {
    static int loaded = 0;
    if (loaded) return;
    loaded = 1;
    gDefaultCpuIntervalMs = env_int("PLUA_CPU_INTERVAL_MS", DEFAULT_CPU_SAMPLE_ITER_MS);
    if (gDefaultCpuIntervalMs <= 0) gDefaultCpuIntervalMs = DEFAULT_CPU_SAMPLE_ITER_MS;
    gDefaultCpuHookCount = env_int("PLUA_CPU_HOOK_COUNT", DEFAULT_CPU_HOOK_COUNT);
    if (gDefaultCpuHookCount <= 0) gDefaultCpuHookCount = DEFAULT_CPU_HOOK_COUNT;
    gDefaultMemProfileRate = env_int("PLUA_MEM_RATE", DEFAULT_MEM_PROFILE_RATE);
    if (gDefaultMemProfileRate <= 0) gDefaultMemProfileRate = DEFAULT_MEM_PROFILE_RATE;
    const char *backend = getenv("PLUA_CPU_BACKEND");
    if (backend && (strcmp(backend, "count") == 0 || strcmp(backend, "maskcount") == 0)) {
        gForceCountBackend = 1;
    }
#if !defined(PLUA_HAS_POSIX_PROF)
    gForceCountBackend = 1;
#endif
}

// Registry userdata (__gc) keeps session lifetime tied to lua_State close (Lua 5.2+).
static const char *kSessionAnchorKey = "plua.session_anchor";

struct SessionAnchor {
    lua_State *owner = NULL;
};

static void destroy_session(ProfilerSession *s); // fwd
static void ensure_session_anchor(lua_State *L); // fwd
static void clear_string_tables(ProfilerSession *s);
static void flush_cpu(ProfilerSession *s);
static void flush_mem(ProfilerSession *s);
static void rearm_session_hook(ProfilerSession *s);

static ProfilerSession *get_session(lua_State *L, bool create) {
    auto it = gSessions.find(L);
    if (it != gSessions.end()) {
        return it->second;
    }
    if (!create) return NULL;
    ProfilerSession *s = new ProfilerSession();
    s->L = L;
    load_env_defaults();
    s->cpuIntervalMs = gDefaultCpuIntervalMs;
    s->cpuHookCount = gDefaultCpuHookCount;
    s->memProfileRate = gDefaultMemProfileRate;
    gSessions[L] = s;
    ensure_session_anchor(L);
    return s;
}

static int session_anchor_gc(lua_State *L) {
    SessionAnchor *a = (SessionAnchor *)lua_touserdata(L, 1);
    if (!a || !a->owner) return 0;
    lua_State *owner = a->owner;
    a->owner = NULL;
    auto it = gSessions.find(owner);
    if (it == gSessions.end()) return 0;
    destroy_session(it->second);
    return 0;
}

static void ensure_session_anchor(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kSessionAnchorKey);
    if (!lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pop(L, 1);

    SessionAnchor *a = (SessionAnchor *)lua_newuserdata(L, sizeof(SessionAnchor));
    a->owner = L;

    if (luaL_newmetatable(L, "plua.session_anchor")) {
        lua_pushcfunction(L, session_anchor_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, kSessionAnchorKey);
}

// Tear down profiling resources and remove session from gSessions.
static void destroy_session(ProfilerSession *s) {
    if (!s) return;
    lua_State *L = s->L;

    s->pendingCpuSample.store(false, std::memory_order_relaxed);
    s->pendingMemSample.store(false, std::memory_order_relaxed);

#if defined(PLUA_HAS_POSIX_PROF)
    if (s->useSigprof) {
        // stop_posix_timer defined later; inline ref-count drop here via flag then call after decl
        s->cpuRunning = false;
    }
#endif
    s->cpuRunning = false;
    s->memRunning = false;

#if defined(PLUA_HAS_POSIX_PROF)
    if (s->useSigprof) {
        if (gPosixTimerUsers > 0) {
            gPosixTimerUsers--;
            if (gPosixTimerUsers == 0) {
                struct itimerval timer;
                memset(&timer, 0, sizeof(timer));
                setitimer(ITIMER_PROF, &timer, NULL);
            }
        }
        s->useSigprof = false;
    }
#endif

    if (L && s->memData.oldAlloc) {
        lua_setallocf(L, s->memData.oldAlloc, s->memData.oldAllocUd);
        s->memData.oldAlloc = NULL;
        s->memData.oldAllocUd = NULL;
    }

    if (L) {
        lua_sethook(L, 0, 0, 0);
    }

    flush_cpu(s);
    flush_mem(s);
    clear_string_tables(s);

    if (L) {
        gSessions.erase(L);
    }
    s->L = NULL;
    delete s;
}

static void release_session_if_idle(ProfilerSession *s) {
    if (!s) return;
    if (s->cpuRunning || s->memRunning) return;
    if (s->cpuData.fd >= 0 || s->memData.alloc_size_fd >= 0 || s->memData.usage_fd >= 0) return;
    if (s->memData.oldAlloc) return;
    destroy_session(s);
}

static void clear_string_tables(ProfilerSession *s) {
    s->string2Id.clear();
    s->id2String.clear();
}

static void init_ignore_names(ProfilerSession *s) {
    for (int i = 0; i < VALID_MIN_ID; i++) {
        s->string2Id[IGNORE_NAME[i]] = i;
        s->id2String[i] = IGNORE_NAME[i];
    }
}

static bool flush_file(int fd, const char *buf, size_t len) {
    if (fd < 0) return false;
    while (len > 0) {
        int r = (int)write(fd, buf, (unsigned int)len);
        if (r < 0) {
            if (errno == EINTR) continue;
            LERR("write fail errno=%d", errno);
            return false;
        }
        if (r == 0) {
            LERR("write returned 0");
            return false;
        }
        buf += r;
        len -= (size_t)r;
    }
    return true;
}

static bool write_pro_header(int fd) {
    uint32_t magic = PLUA_PRO_MAGIC;
    uint32_t version = PLUA_PRO_VERSION;
    if (!flush_file(fd, (const char *)&magic, sizeof(magic))) return false;
    if (!flush_file(fd, (const char *)&version, sizeof(version))) return false;
    return true;
}

// Walk the Lua debug stack bottom→top. Like vLua (which only keeps CallInfo
// frames with CIST_LUA), skip pure C frames and record Lua/main frames so a
// sample that lands while a C API / C callback is active still attributes to
// the nearest/top Lua caller (source/name/line via lua_getinfo).
static void get_cur_callstack(ProfilerSession *s, CallStack &cs) {
    lua_State *L = s->L;
    lua_Debug ar;
    int last = lastlevel(L);
    int i = 0;
    cs.depth = 0;

    while (lua_getstack(L, last, &ar) && i < MAX_STACK_SIZE) {
        lua_getinfo(L, "Slnt", &ar);
        i++;
        last--;

        // Skip C frames — recover surrounding Lua attribution (vLua approach
        // adapted to the public debug API).
        if (ar.what != NULL && ar.what[0] == 'C') {
            continue;
        }

        auto funcname = get_funcname(L, &ar);

        int id = 0;
        auto iter = s->string2Id.find(funcname);
        if (iter == s->string2Id.end()) {
            id = (int)s->string2Id.size();
            s->string2Id[funcname] = id;
            s->id2String[id] = funcname;
        } else {
            id = iter->second;
        }

        if (id < VALID_MIN_ID) {
            continue;
        }

        LLOG("%s %d %d", funcname.c_str(), id, last);
        cs.stack[cs.depth] = id;
        cs.depth++;
    }
}

static bool flush_string_table(ProfilerSession *s, int fd) {
    int total_len = 0;
    for (auto iter = s->string2Id.begin(); iter != s->string2Id.end(); iter++) {
        const std::string &str = iter->first;
        int id = iter->second;
        if (id < VALID_MIN_ID) continue;

        int len = (int)str.length();
        len = len > MAX_FUNC_NAME_SIZE ? MAX_FUNC_NAME_SIZE : len;
        if (!flush_file(fd, str.c_str(), (size_t)len)) return false;
        if (!flush_file(fd, (const char *)&len, sizeof(len))) return false;
        if (!flush_file(fd, (const char *)&id, sizeof(id))) return false;
        total_len++;
    }
    if (!flush_file(fd, (const char *)&total_len, sizeof(total_len))) return false;
    return true;
}

static void flush_cpu(ProfilerSession *s) {
    if (s->cpuData.total <= 0) {
        if (s->cpuData.fd >= 0) {
            close(s->cpuData.fd);
            s->cpuData.fd = -1;
        }
        return;
    }

    LLOG("flush cpu...");
    bool ok = true;
    for (auto iter = s->cpuData.callstack.begin(); iter != s->cpuData.callstack.end(); iter++) {
        const CallStack &cs = iter->first;
        int count = iter->second;
        if (!flush_file(s->cpuData.fd, (const char *)&count, sizeof(count))) { ok = false; break; }
        if (!flush_file(s->cpuData.fd, (const char *)&cs, sizeof(cs))) { ok = false; break; }
    }
    if (ok) {
        ok = flush_string_table(s, s->cpuData.fd);
    }

    int total = s->cpuData.total;
    s->cpuData.total = 0;
    s->cpuData.callstack.clear();

    if (s->cpuData.fd >= 0) {
        close(s->cpuData.fd);
        s->cpuData.fd = -1;
    }

    if (ok) {
        printf("pLua flush ok %d\n", total);
    } else {
        printf("pLua flush failed (write error), samples=%d\n", total);
    }
}

static std::string get_mem_filename(std::string path, const std::string &suffix) {
    auto pos = path.find_last_of('.');
    if (pos == std::string::npos) {
        return path + suffix;
    }
    path.insert(pos, suffix);
    return path;
}

static void flush_mem_left(ProfilerSession *s, int fd) {
    flush_string_table(s, fd);
    if (fd >= 0) {
        close(fd);
    }
}

static void flush_mem_alloc_size(ProfilerSession *s) {
    int fd = s->memData.alloc_size_fd;
    for (auto iter = s->memData.callstack.begin(); iter != s->memData.callstack.end(); iter++) {
        const CallStack &cs = *iter->first;
        auto &mem_info = iter->second;
        flush_file(fd, (const char *)&mem_info.allocs, sizeof(mem_info.allocs));
        flush_file(fd, (const char *)&cs, sizeof(cs));
    }
    flush_mem_left(s, fd);
    s->memData.alloc_size_fd = -1;
}

static void flush_mem_usage(ProfilerSession *s) {
    int fd = s->memData.usage_fd;
    for (auto iter = s->memData.callstack.begin(); iter != s->memData.callstack.end(); iter++) {
        const CallStack &cs = *iter->first;
        auto &mem_info = iter->second;
        auto usage = (int)mem_info.allocs - (int)mem_info.frees;
        if (usage <= 0) continue;
        flush_file(fd, (const char *)&usage, sizeof(usage));
        flush_file(fd, (const char *)&cs, sizeof(cs));
    }
    flush_mem_left(s, fd);
    s->memData.usage_fd = -1;
}

static void flush_mem(ProfilerSession *s) {
    if (s->memData.total <= 0) {
        if (s->memData.alloc_size_fd >= 0) { close(s->memData.alloc_size_fd); s->memData.alloc_size_fd = -1; }
        if (s->memData.usage_fd >= 0) { close(s->memData.usage_fd); s->memData.usage_fd = -1; }
        return;
    }

    LLOG("flush mem...");
    flush_mem_alloc_size(s);
    flush_mem_usage(s);

    int total = s->memData.total;
    s->memData.total = 0;
    for (auto iter = s->memData.callstack.begin(); iter != s->memData.callstack.end(); iter++) {
        delete iter->first;
    }
    s->memData.callstack.clear();
    s->memData.ptr2Callstack.clear();
    printf("pLua flush ok %d\n", total);
}

static void take_cpu_sample(ProfilerSession *s) {
    if (!s->cpuRunning) return;
    if (s->cpuSampleLimit != 0 && s->cpuSampleLimit <= s->cpuData.total) {
        return;
    }
    CallStack cs;
    get_cur_callstack(s, cs);
    // No Lua frame recoverable (e.g. pure C with empty debug stack) — drop.
    if (cs.depth <= 0) return;
    s->cpuData.callstack[cs]++;
    s->cpuData.total++;
}

static void take_mem_sample(ProfilerSession *s) {
    s->memData.is_in_hook++;
    auto hook_alloc_ptr = s->memData.hook_alloc_ptr;
    auto hook_alloc_sz = s->memData.hook_alloc_sz;

    CallStack cs;
    get_cur_callstack(s, cs);
    if (cs.depth <= 0) {
        s->memData.is_in_hook--;
        return;
    }

    auto it = s->memData.callstack.find(&cs);
    CallStack *pointer_cs = 0;
    if (it == s->memData.callstack.end()) {
        auto new_cs = new CallStack();
        memcpy(new_cs, &cs, sizeof(cs));
        s->memData.callstack[new_cs] = MemInfo(1, 0);
        pointer_cs = new_cs;
    } else {
        it->second.allocs++;
        pointer_cs = it->first;
    }
    s->memData.ptr2Callstack[hook_alloc_ptr] = pointer_cs;
    LLOG("alloc %p %u", hook_alloc_ptr, (unsigned)hook_alloc_sz);
    s->memData.is_in_hook--;
}

static void SessionHook(lua_State *L, lua_Debug *par) {
    (void)par;
    ProfilerSession *s = get_session(L, false);
    if (!s || s->L != L) {
        lua_sethook(L, 0, 0, 0);
        return;
    }

    bool didCpu = s->pendingCpuSample.exchange(false, std::memory_order_relaxed);
    bool didMem = s->pendingMemSample.exchange(false, std::memory_order_relaxed);

    if (didCpu) {
        if (s->cpuRunning && (s->cpuSampleLimit == 0 || s->cpuSampleLimit > s->cpuData.total)) {
            take_cpu_sample(s);
        }
        if (s->cpuRunning && s->cpuSampleLimit != 0 && s->cpuSampleLimit <= s->cpuData.total) {
            // stop cpu when limit hit — defer actual stop to avoid reentrancy issues
            // Mark and stop safely below after mem
            s->cpuRunning = false;
#if defined(PLUA_HAS_POSIX_PROF)
            if (s->useSigprof && gPosixTimerUsers > 0) {
                gPosixTimerUsers--;
                if (gPosixTimerUsers == 0) {
                    struct itimerval timer;
                    memset(&timer, 0, sizeof(timer));
                    setitimer(ITIMER_PROF, &timer, NULL);
                }
            }
#endif
            flush_cpu(s);
            if (!s->memRunning) {
                clear_string_tables(s);
                release_session_if_idle(s);
                return; // session may be deleted
            }
        }
    }
    if (didMem) {
        take_mem_sample(s);
    }

    rearm_session_hook(s);
}

static void rearm_session_hook(ProfilerSession *s) {
    if (!s || !s->L) return;
    if (s->pendingCpuSample.load(std::memory_order_relaxed) ||
        s->pendingMemSample.load(std::memory_order_relaxed)) {
        lua_sethook(s->L, SessionHook, LUA_MASKCOUNT, 1);
        return;
    }
    // Count-backend continuous sampling: every N VM instructions becomes one sample.
    if (s->cpuRunning && !s->useSigprof) {
        s->pendingCpuSample.store(true, std::memory_order_relaxed);
        lua_sethook(s->L, SessionHook, LUA_MASKCOUNT, s->cpuHookCount);
        return;
    }
    // SIGPROF backend: keep a drain hook armed. Signal only sets pendingCpuSample;
    // actual sampling happens here (never lua_sethook from the signal handler).
    if (s->cpuRunning && s->useSigprof) {
        int step = s->cpuHookCount > 0 ? s->cpuHookCount : DEFAULT_CPU_HOOK_COUNT;
        lua_sethook(s->L, SessionHook, LUA_MASKCOUNT, step);
        return;
    }
    lua_sethook(s->L, 0, 0, 0);
}

static void request_hook(ProfilerSession *s, bool cpu, bool mem) {
    // Called from Lua alloc path / safe contexts — NOT from the POSIX signal handler.
    if (cpu) s->pendingCpuSample.store(true, std::memory_order_relaxed);
    if (mem) s->pendingMemSample.store(true, std::memory_order_relaxed);
    if (s->L) {
        lua_sethook(s->L, SessionHook, LUA_MASKCOUNT, 1);
    }
}

#if defined(PLUA_HAS_POSIX_PROF)
static void SignalHandler(int sig, siginfo_t *sinfo, void *ucontext) {
    (void)sig; (void)sinfo; (void)ucontext;
    // Async-signal-safe path: only mark sample pending. Do NOT call lua_sethook
    // or any Lua / allocator / non-AS-safe API here (historical crash risk #8).
    for (auto &kv : gSessions) {
        ProfilerSession *s = kv.second;
        if (s && s->L && s->cpuRunning && s->useSigprof) {
            s->pendingCpuSample.store(true, std::memory_order_relaxed);
        }
    }
}

static int ensure_sigprof_handler() {
    if (gSigHandlerInstalled) return 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = SignalHandler;
    sa.sa_flags = SA_RESTART | SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL) == -1) {
        LERR("sigaction(SIGPROF) failed");
        return -1;
    }
    gSigHandlerInstalled = true;
    return 0;
}

static int start_posix_timer(ProfilerSession *s) {
    if (ensure_sigprof_handler() != 0) return -1;
    if (gPosixTimerUsers == 0) {
        struct itimerval timer;
        timer.it_interval.tv_sec = 0;
        timer.it_interval.tv_usec = s->cpuIntervalMs * 1000;
        timer.it_value = timer.it_interval;
        int ret = setitimer(ITIMER_PROF, &timer, NULL);
        if (ret != 0) {
            LERR("setitimer fail %d", ret);
            return -1;
        }
    }
    gPosixTimerUsers++;
    return 0;
}

static void stop_posix_timer(ProfilerSession *s) {
    (void)s;
    if (gPosixTimerUsers <= 0) return;
    gPosixTimerUsers--;
    if (gPosixTimerUsers == 0) {
        struct itimerval timer;
        memset(&timer, 0, sizeof(timer));
        setitimer(ITIMER_PROF, &timer, NULL);
    }
}
#endif

static int start_cpu_backend(ProfilerSession *s) {
#if defined(PLUA_HAS_POSIX_PROF)
    if (!gForceCountBackend) {
        s->useSigprof = true;
        return start_posix_timer(s);
    }
#endif
    s->useSigprof = false;
    s->pendingCpuSample = false;
    // SessionHook / rearm_session_hook arms LUA_MASKCOUNT sampling.
    return 0;
}

static void stop_cpu_backend(ProfilerSession *s) {
#if defined(PLUA_HAS_POSIX_PROF)
    if (s->useSigprof) {
        stop_posix_timer(s);
    }
#endif
    s->useSigprof = false;
    // Only clear hook if mem is not pending work; rearm handles it
    rearm_session_hook(s);
}

static int lrealstopsafe(lua_State *L) {
    ProfilerSession *s = get_session(L, false);
    if (!s) {
        return 0;
    }
    // lrealstop may have cleared cpuRunning already (async stop / sample limit); still tear down.
    bool was_running = s->cpuRunning || s->cpuData.fd >= 0 || s->cpuData.total > 0 || s->useSigprof;
    if (was_running) {
        s->cpuRunning = false;
        stop_cpu_backend(s);
        flush_cpu(s);
        if (!s->memRunning) {
            clear_string_tables(s);
        }
    }
    if (!s->memRunning) {
        release_session_if_idle(s);
    }
    return 0;
}

extern "C" int lrealstop(lua_State *L) {
    ProfilerSession *s = get_session(L, false);
    if (s) s->cpuRunning = false;
    return 0;
}

static int lrealstartsafe(lua_State *L) {
    ProfilerSession *s = get_session(L, false);
    if (!s) return -1;
    if (s->cpuRunning) {
        LERR("cpu start again, failed");
        return -1;
    }

    if (s->string2Id.empty()) {
        init_ignore_names(s);
    }

    int fd = open(s->cpuFilename.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        LERR("open file fail %s", s->cpuFilename.c_str());
        return -1;
    }
    if (!write_pro_header(fd)) {
        close(fd);
        LERR("write header fail %s", s->cpuFilename.c_str());
        return -1;
    }
    s->cpuData.fd = fd;
    s->cpuData.total = 0;
    s->cpuData.callstack.clear();

    if (start_cpu_backend(s) != 0) {
        close(fd);
        s->cpuData.fd = -1;
        return -1;
    }
    s->cpuRunning = true;
    return 0;
}

static void StartHandlerHook(lua_State *L, lua_Debug *par) {
    (void)par;
    lrealstartsafe(L);
    ProfilerSession *s = get_session(L, false);
    rearm_session_hook(s);
}

extern "C" int lrealstart(lua_State *L, int second, const char *file) {
    load_env_defaults();
    ProfilerSession *s = get_session(L, true);
    if (s->cpuRunning) {
        LERR("cpu start again, failed");
        return -1;
    }
    if (!file) {
        LERR("cpu start missing file");
        return -1;
    }

    s->cpuIntervalMs = gDefaultCpuIntervalMs;
    s->cpuHookCount = gDefaultCpuHookCount;
    s->cpuSampleLimit = (second > 0) ? (second * 1000 / s->cpuIntervalMs) : 0;
    s->cpuFilename = file;

    lua_sethook(L, StartHandlerHook, LUA_MASKCOUNT, 1);
    LLOG("lstart %u %s", s->cpuSampleLimit, file);
    return 0;
}

static int lstart(lua_State *L) {
    int second = (int)lua_tointeger(L, 1);
    const char *file = lua_tostring(L, 2);
    if (lua_gettop(L) >= 3 && lua_isnumber(L, 3)) {
        int ms = (int)lua_tointeger(L, 3);
        if (ms > 0) gDefaultCpuIntervalMs = ms;
    }
    if (!file) {
        lua_pushinteger(L, -1);
        lua_pushstring(L, "cpu start failed: missing output path");
        return 2;
    }
    ProfilerSession *existing = get_session(L, false);
    if (existing && existing->cpuRunning) {
        lua_pushinteger(L, -1);
        lua_pushstring(L, "cpu start failed: already running on this lua_State");
        return 2;
    }
    int ret = lrealstart(L, second, file);
    lua_pushinteger(L, ret);
    if (ret != 0) {
        lua_pushstring(L, "cpu start failed: could not start profiler (file or backend error)");
        return 2;
    }
    return 1;
}

static int lstop(lua_State *L) {
    ProfilerSession *s = get_session(L, false);
    LLOG("lstop %s", s ? s->cpuFilename.c_str() : "");
    int ret = lrealstop(L);
    lrealstopsafe(L);
    lua_pushinteger(L, ret);
    return 1;
}

//////////////////////////////////mem profiler////////////////////////////////////////

static uint64_t next_random(uint64_t rnd) {
    const uint64_t prng_mult = 0x5DEECE66DULL;
    const uint64_t prng_add = 0xB;
    const uint64_t prng_mod_power = 48;
    const uint64_t prng_mod_mask = ~((~static_cast<uint64_t>(0)) << prng_mod_power);
    return (prng_mult * rnd + prng_add) & prng_mod_mask;
}

#define MAX_SSIZE (static_cast<ssize_t>(static_cast<size_t>(static_cast<ssize_t>(-1)) >> 1))

static ssize_t gen_next_sample(ProfilerSession *s) {
    s->memData.rand = next_random(s->memData.rand);
    const uint64_t prng_mod_power = 48;
    double q = static_cast<uint32_t>(s->memData.rand >> (prng_mod_power - 26)) + 1.0;
    double interval = (log2(q) - 26) * (-log(2.0) * s->memProfileRate);
    return static_cast<ssize_t>(std::min<double>(interval, static_cast<double>(MAX_SSIZE)));
}

static int lrealstopmemsafe(lua_State *L) {
    ProfilerSession *s = get_session(L, false);
    if (!s) {
        return 0;
    }
    bool was_running = s->memRunning || s->memData.oldAlloc != NULL ||
                       s->memData.alloc_size_fd >= 0 || s->memData.usage_fd >= 0;
    if (!was_running) {
        return 0;
    }

    if (s->memData.oldAlloc) {
        lua_setallocf(L, s->memData.oldAlloc, s->memData.oldAllocUd);
        s->memData.oldAlloc = NULL;
        s->memData.oldAllocUd = NULL;
    }

    s->memRunning = false;
    flush_mem(s);
    if (!s->cpuRunning) {
        clear_string_tables(s);
        rearm_session_hook(s);
        release_session_if_idle(s);
    }
    return 0;
}

extern "C" int lrealstopmem(lua_State *L) {
    ProfilerSession *s = get_session(L, false);
    if (s) s->memRunning = false;
    return 0;
}

static void *my_lua_Alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    ProfilerSession *s = (ProfilerSession *)ud;
    size_t realosize = (ptr) ? osize : 0;

    if (!s->memRunning) {
        void *ret = s->memData.oldAlloc(s->memData.oldAllocUd, ptr, osize, nsize);
        lrealstopmemsafe(s->L);
        return ret;
    }

    if (realosize < nsize) {
        LLOG("my_lua_Alloc alloc %p %p %u %u", ud, ptr, (unsigned)osize, (unsigned)nsize);

        if (s->memData.is_in_hook) {
            return s->memData.oldAlloc(s->memData.oldAllocUd, ptr, osize, nsize);
        }

        if (s->memSampleLimit != 0 && s->memSampleLimit <= s->memData.total) {
            LLOG("lrealstopmem...");
            lrealstopmem(s->L);
            return s->memData.oldAlloc(s->memData.oldAllocUd, ptr, osize, nsize);
        }

        size_t alloc_sz = nsize - realosize;
        if (realosize > 0 && ptr) {
            s->memData.ptr2Callstack.erase(ptr);
        }

        if (static_cast<ssize_t>(alloc_sz) < s->memData.nextSample) {
            s->memData.nextSample -= static_cast<ssize_t>(alloc_sz);
            return s->memData.oldAlloc(s->memData.oldAllocUd, ptr, osize, nsize);
        }

        s->memData.nextSample = gen_next_sample(s);
        s->memData.total++;

        void *alloc_ptr = s->memData.oldAlloc(s->memData.oldAllocUd, ptr, osize, nsize);
        s->memData.hook_alloc_ptr = alloc_ptr;
        s->memData.hook_alloc_sz = alloc_sz;
        request_hook(s, false, true);
        return alloc_ptr;
    } else if (realosize > nsize) {
        auto it = s->memData.ptr2Callstack.find(ptr);
        if (it == s->memData.ptr2Callstack.end()) {
            return s->memData.oldAlloc(s->memData.oldAllocUd, ptr, osize, nsize);
        }

        CallStack *cs = it->second;
        s->memData.ptr2Callstack.erase(it);

        auto it2 = s->memData.callstack.find(cs);
        if (it2 != s->memData.callstack.end()) {
            it2->second.frees++;
        }

        void *ret = s->memData.oldAlloc(s->memData.oldAllocUd, ptr, osize, nsize);
        if (nsize > 0 && ret) {
            s->memData.ptr2Callstack[ret] = cs;
        }
        return ret;
    } else {
        return s->memData.oldAlloc(s->memData.oldAllocUd, ptr, osize, nsize);
    }
}

static int lrealstartmemsafe(lua_State *L) {
    ProfilerSession *s = get_session(L, false);
    if (!s) return -1;
    if (s->memRunning) {
        LERR("mem start again, failed");
        return -1;
    }

    if (s->string2Id.empty()) {
        init_ignore_names(s);
    }

    int fd = open(get_mem_filename(s->memFilename, "_ALLOC_SIZE").c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        LERR("open file fail %s", s->memFilename.c_str());
        return -1;
    }
    if (!write_pro_header(fd)) { close(fd); return -1; }
    s->memData.alloc_size_fd = fd;

    fd = open(get_mem_filename(s->memFilename, "_USAGE").c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        LERR("open file fail %s", s->memFilename.c_str());
        close(s->memData.alloc_size_fd);
        s->memData.alloc_size_fd = -1;
        return -1;
    }
    if (!write_pro_header(fd)) {
        close(fd);
        close(s->memData.alloc_size_fd);
        s->memData.alloc_size_fd = -1;
        return -1;
    }
    s->memData.usage_fd = fd;

    s->memData.total = 0;
    for (auto iter = s->memData.callstack.begin(); iter != s->memData.callstack.end(); iter++) {
        delete iter->first;
    }
    s->memData.callstack.clear();
    s->memData.ptr2Callstack.clear();

    s->memData.oldAlloc = lua_getallocf(L, &s->memData.oldAllocUd);
    lua_setallocf(L, my_lua_Alloc, s);
    s->memData.rand = (uint64_t)time(NULL);
    for (int i = 0; i < 20; i++) {
        s->memData.rand = next_random(s->memData.rand);
    }
    s->memData.nextSample = gen_next_sample(s);
    s->memData.is_in_hook = 0;
    s->memData.hook_alloc_ptr = 0;
    s->memData.hook_alloc_sz = 0;
    s->memRunning = true;
    return 0;
}

static void StartMemHandlerHook(lua_State *L, lua_Debug *par) {
    (void)par;
    lrealstartmemsafe(L);
    ProfilerSession *s = get_session(L, false);
    rearm_session_hook(s);
}

extern "C" int lrealstartmem(lua_State *L, int count, const char *file) {
    load_env_defaults();
    ProfilerSession *s = get_session(L, true);
    if (s->memRunning) {
        LERR("mem start again, failed");
        return -1;
    }
    if (!file) {
        LERR("mem start missing file");
        return -1;
    }

    s->memProfileRate = gDefaultMemProfileRate;
    s->memSampleLimit = count;
    s->memFilename = file;
    s->memData.oldAlloc = NULL;
    s->memData.oldAllocUd = NULL;
    s->memData.nextSample = 0;

    lua_sethook(L, StartMemHandlerHook, LUA_MASKCOUNT, 1);
    LLOG("lstartmem %u %s", s->memSampleLimit, file);
    return 0;
}

static int lstart_mem(lua_State *L) {
    int count = (int)lua_tointeger(L, 1);
    const char *file = lua_tostring(L, 2);
    if (lua_gettop(L) >= 3 && lua_isnumber(L, 3)) {
        int rate = (int)lua_tointeger(L, 3);
        if (rate > 0) gDefaultMemProfileRate = rate;
    }
    if (!file) {
        lua_pushinteger(L, -1);
        lua_pushstring(L, "mem start failed: missing output path");
        return 2;
    }
    ProfilerSession *existing = get_session(L, false);
    if (existing && existing->memRunning) {
        lua_pushinteger(L, -1);
        lua_pushstring(L, "mem start failed: already running on this lua_State");
        return 2;
    }
    int ret = lrealstartmem(L, count, file);
    lua_pushinteger(L, ret);
    if (ret != 0) {
        lua_pushstring(L, "mem start failed: could not start profiler (file or setup error)");
        return 2;
    }
    return 1;
}

static int lstop_mem(lua_State *L) {
    ProfilerSession *s = get_session(L, false);
    LLOG("lstop_mem %s", s ? s->memFilename.c_str() : "");
    int ret = lrealstopmem(L);
    lrealstopmemsafe(L);
    lua_pushinteger(L, ret);
    return 1;
}

static void push_config_table(lua_State *L) {
    lua_newtable(L);
    lua_pushinteger(L, gDefaultCpuIntervalMs);
    lua_setfield(L, -2, "cpu_interval_ms");
    lua_pushinteger(L, gDefaultCpuHookCount);
    lua_setfield(L, -2, "cpu_hook_count");
    lua_pushinteger(L, gDefaultMemProfileRate);
    lua_setfield(L, -2, "mem_rate");
#if defined(PLUA_HAS_POSIX_PROF)
    lua_pushstring(L, gForceCountBackend ? "count" : "sigprof");
#else
    lua_pushstring(L, "count");
#endif
    lua_setfield(L, -2, "cpu_backend");
}

static int lstatus(lua_State *L) {
    load_env_defaults();
    ProfilerSession *s = get_session(L, false);

    lua_newtable(L);
    lua_pushboolean(L, s && s->cpuRunning);
    lua_setfield(L, -2, "cpu_running");
    lua_pushboolean(L, s && s->memRunning);
    lua_setfield(L, -2, "mem_running");
    lua_pushinteger(L, s ? s->cpuData.total : 0);
    lua_setfield(L, -2, "cpu_samples");
    lua_pushinteger(L, s ? s->memData.total : 0);
    lua_setfield(L, -2, "mem_samples");

    if (s && !s->cpuFilename.empty()) {
        lua_pushstring(L, s->cpuFilename.c_str());
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -2, "cpu_output");

    if (s && !s->memFilename.empty()) {
        lua_pushstring(L, s->memFilename.c_str());
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -2, "mem_output");

    if (s && s->cpuRunning) {
        lua_pushstring(L, s->useSigprof ? "sigprof" : "count");
    } else {
#if defined(PLUA_HAS_POSIX_PROF)
        lua_pushstring(L, gForceCountBackend ? "count" : "sigprof");
#else
        lua_pushstring(L, "count");
#endif
    }
    lua_setfield(L, -2, "cpu_backend");

    push_config_table(L);
    lua_setfield(L, -2, "config");
    return 1;
}

// Explicit session teardown (also happens automatically on lua_close via registry __gc,
// and when both cpu/mem are fully stopped).
static int lclose(lua_State *L) {
    ProfilerSession *s = get_session(L, false);
    if (!s) {
        lua_pushinteger(L, 0);
        return 1;
    }
    if (s->cpuRunning || s->cpuData.fd >= 0 || s->cpuData.total > 0) {
        lrealstop(L);
        lrealstopsafe(L);
        s = get_session(L, false);
    }
    if (s && (s->memRunning || s->memData.oldAlloc ||
              s->memData.alloc_size_fd >= 0 || s->memData.usage_fd >= 0)) {
        lrealstopmem(L);
        lrealstopmemsafe(L);
        s = get_session(L, false);
    }
    if (s) {
        destroy_session(s);
    }
    lua_pushinteger(L, 0);
    return 1;
}

static int lset_config(lua_State *L) {
    load_env_defaults();
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "cpu_interval_ms");
        if (lua_isnumber(L, -1)) {
            int v = (int)lua_tointeger(L, -1);
            if (v > 0) gDefaultCpuIntervalMs = v;
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "cpu_hook_count");
        if (lua_isnumber(L, -1)) {
            int v = (int)lua_tointeger(L, -1);
            if (v > 0) gDefaultCpuHookCount = v;
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "mem_rate");
        if (lua_isnumber(L, -1)) {
            int v = (int)lua_tointeger(L, -1);
            if (v > 0) gDefaultMemProfileRate = v;
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "cpu_backend");
        if (lua_isstring(L, -1)) {
            const char *b = lua_tostring(L, -1);
            if (b && (strcmp(b, "count") == 0 || strcmp(b, "maskcount") == 0)) {
                gForceCountBackend = 1;
            } else if (b && strcmp(b, "sigprof") == 0) {
#if defined(PLUA_HAS_POSIX_PROF)
                gForceCountBackend = 0;
#endif
            }
        }
        lua_pop(L, 1);
    }
    lua_pushinteger(L, 0);
    return 1;
}

static int lget_config(lua_State *L) {
    load_env_defaults();
    push_config_table(L);
    return 1;
}

static void register_plua_api(lua_State *L) {
    luaL_Reg l[] = {
            {"start",      lstart},
            {"stop",       lstop},
            {"start_mem",  lstart_mem},
            {"stop_mem",   lstop_mem},
            {"status",     lstatus},
            {"close",      lclose},
            {"set_config", lset_config},
            {"get_config", lget_config},
            {NULL, NULL},
    };
    luaL_newlib(L, l);
}

extern "C" int luaopen_libplua(lua_State *L) {
    luaL_checkversion(L);
    register_plua_api(L);
    return 1;
}

// Alias so `require "plua"` also works when the shared library is named accordingly.
extern "C" int luaopen_plua(lua_State *L) {
    return luaopen_libplua(L);
}
