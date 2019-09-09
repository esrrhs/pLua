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
#include <graphviz/gvc.h>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

int open_debug = 0;
int gsamplecount;
std::string gfilename;
lua_State *gL;
int grunning = 0;

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
    /* find an upper bound */
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

static const int VALID_MIN_ID = 3;

static const int MAX_STACK_SIZE = 64;
static const int MAX_CALL_STACK_SIZE = 4;
static const int MAX_BUCKET_SIZE = 1 << 10;
static const int MAX_CALL_STACK_SAVE_SIZE = 1 << 18;

struct CallStack {
    int count;
    int depth;
    int stack[MAX_STACK_SIZE];
};

struct Bucket {
    CallStack cs[MAX_CALL_STACK_SIZE];
};

struct ProfileData {
    Bucket bucket[MAX_BUCKET_SIZE];
    int total;
};

int gfd;

ProfileData gProfileData;
CallStack gCallStackSaved[MAX_CALL_STACK_SAVE_SIZE];
int gCallStackSavedSize = 0;


static void flush_file(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t r = write(fd, buf, len);
        buf += r;
        len -= r;
    }
}

static void flush_callstack() {
    LLOG("flush_callstack");
    flush_file(gfd, (const char *) gCallStackSaved, sizeof(CallStack) * gCallStackSavedSize);
    gCallStackSavedSize = 0;
}

static void save_callstack(CallStack *pcs) {

    LLOG("save_callstack");

    if (gCallStackSavedSize >= MAX_CALL_STACK_SAVE_SIZE) {
        flush_callstack();
    }
    gCallStackSaved[gCallStackSavedSize] = *pcs;
    gCallStackSavedSize++;
}

static void flush() {
    if (gProfileData.total <= 0) {
        return;
    }

    LLOG("flush...");

    int total = 0;
    for (int b = 0; b < MAX_BUCKET_SIZE; b++) {
        Bucket *bucket = &gProfileData.bucket[b];
        for (int a = 0; a < MAX_CALL_STACK_SIZE; a++) {
            if (bucket->cs[a].count > 0) {
                save_callstack(&bucket->cs[a]);
                bucket->cs[a].depth = 0;
                bucket->cs[a].count = 0;
                total++;
            }
        }
    }

    flush_callstack();

    for (auto iter = gString2Id.begin(); iter != gString2Id.end(); iter++) {
        const std::string &str = iter->first;
        int id = iter->second;

        int len = str.length();
        len = len > MAX_FUNC_NAME_SIZE ? MAX_FUNC_NAME_SIZE : len;
        flush_file(gfd, str.c_str(), len);
        flush_file(gfd, (const char *) &len, sizeof(len));

        flush_file(gfd, (const char *) &id, sizeof(id));
    }

    int len = gString2Id.size();
    flush_file(gfd, (const char *) &len, sizeof(len));

    LLOG("flush ok %d %d", total, gProfileData.total);

    gProfileData.total = 0;

    if (gfd != 0) {
        close(gfd);
        gfd = 0;
    }

    printf("pLua flush ok\n");
}

static int lrealstop() {

    grunning = 0;

    struct itimerval timer;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0;
    timer.it_value = timer.it_interval;
    int ret = setitimer(ITIMER_REAL, &timer, NULL);
    if (ret != 0) {
        LERR("setitimer fail %d", ret);
        return ret;
    }

    flush();
    return 0;
}

static void SignalHandlerHook(lua_State *L, lua_Debug *par) {

    LLOG("Hook...");

    lua_sethook(gL, 0, 0, 0);

    if (gsamplecount != 0 && gsamplecount <= gProfileData.total) {
        LLOG("lrealstop...");
        lrealstop();
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
		
		if (id < VALID_MIN_ID)
		{
			continue;
		}

        LLOG("%s %d %d", funcname, id, last);

        cs.stack[cs.depth] = id;
        cs.depth++;
    }

    int hash = 0;
    for (int i = 0; i < cs.depth; i++) {
        int id = cs.stack[i];
        hash = (hash << 8) | (hash >> (8 * (sizeof(hash) - 1)));
        hash += (id * 31) + (id * 7) + (id * 3);
    }

    LLOG("hash %d", hash);

    bool done = false;
    Bucket *bucket = &gProfileData.bucket[(uint32_t) hash % MAX_BUCKET_SIZE];
    for (int a = 0; a < MAX_CALL_STACK_SIZE; a++) {
        CallStack *pcs = &bucket->cs[a];
        if (pcs->depth == 0 && pcs->count == 0) {
            pcs->depth = cs.depth;
            pcs->count = 1;
            memcpy(pcs->stack, cs.stack, sizeof(int) * cs.depth);
            done = true;

            LLOG("hash %d add first %d %d", hash, pcs->count, pcs->depth);
            break;
        } else if (pcs->depth == cs.depth) {
            if (memcmp(pcs->stack, cs.stack, sizeof(int) * cs.depth) != 0) {
                break;
            } else {
                pcs->count++;
                done = true;

                LLOG("hash %d add %d %d", hash, pcs->count, pcs->depth);
                break;
            }
        }
    }

    if (!done) {
        CallStack *pcs = &bucket->cs[0];
        for (int a = 1; a < MAX_CALL_STACK_SIZE; a++) {
            if (bucket->cs[a].count < pcs->count) {
                pcs = &bucket->cs[a];
            }
        }

        if (pcs->count > 0) {
            save_callstack(pcs);
        }

        // Use the newly evicted entry
        pcs->depth = cs.depth;
        pcs->count = 1;
        memcpy(pcs->stack, cs.stack, sizeof(int) * cs.depth);

        LLOG("hash %d add new %d %d", hash, pcs->count, pcs->depth);
    }

}

static void SignalHandler(int sig, siginfo_t *sinfo, void *ucontext) {
	// hack lua5.3.4 linux-x64 为了判断是否不在lua中 L-nny == 0 && L-nCcalls == 0
	unsigned short nny = *(unsigned short *)((char*)gL+196);
	unsigned short nCcalls = *(unsigned short *)((char*)gL+198);
	if (nny == 0 && nCcalls == 0)
	{
		return;
	}
    lua_sethook(gL, SignalHandlerHook, LUA_MASKCOUNT, 1);
}

static int lrealstart(lua_State *L, int second, const char *file) {

    if (grunning) {
        LERR("start again, failed");
        return -1;
    }
    grunning = 1;
	
	gString2Id["?"] = 0;
	gId2String[0] = "?";
	gString2Id["function 'xpcall'"] = 1;
	gId2String[1] = "function 'xpcall'";
	gString2Id["function 'pcall'"] = 2;
	gId2String[2] = "function 'pcall'";

    const int iter = 100;

    gsamplecount = second * 1000 / iter;
    gfilename = file;
    gL = L;

    LLOG("lstart %u %s", gsamplecount, file);

    struct sigaction sa;
    sa.sa_sigaction = SignalHandler;
    sa.sa_flags = SA_RESTART | SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGALRM, &sa, NULL) == -1) {
        LERR("sigaction(SIGALRM) failed");
        return -1;
    }

    struct itimerval timer;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = iter * 1000;
    timer.it_value = timer.it_interval;
    int ret = setitimer(ITIMER_REAL, &timer, NULL);
    if (ret != 0) {
        LERR("setitimer fail %d", ret);
        return -1;
    }

    memset(&gProfileData, 0, sizeof(gProfileData));
    memset(&gCallStackSaved, 0, sizeof(gCallStackSaved));
    memset(&gCallStackSavedSize, 0, sizeof(gCallStackSavedSize));

    int fd = open(file, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        LERR("open file fail %s", file);
        return -1;
    }

    gfd = fd;

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

    LLOG("lstop %s", gfilename.c_str());
    int ret = lrealstop();

    lua_pushinteger(L, ret);
    return 1;
}

static void load_file(int fd, char *buf, size_t len) {
    while (len > 0) {
        ssize_t r = read(fd, buf, len);
        buf += r;
        len -= r;
    }
}

std::vector <CallStack> gLoadCallStack;

static int load(const char *srcfile) {
    int fd = open(srcfile, O_RDONLY, 0666);
    if (fd < 0) {
        LERR("open srcfile fail %s", srcfile);
        return -1;
    }

    int cnt = lseek(fd, 0, SEEK_END);

    int i = 0;

    int namemaplen = 0;
    i += sizeof(namemaplen);
    lseek(fd, -i, SEEK_END);
    load_file(fd, (char *) &namemaplen, sizeof(namemaplen));

    LLOG("namemaplen %d", namemaplen);

    int namenum = 0;
    while (i < cnt && namenum < namemaplen) {
        int id = 0;
        i += sizeof(id);
        lseek(fd, -i, SEEK_END);
        load_file(fd, (char *) &id, sizeof(id));

        int namelen = 0;
        i += sizeof(namelen);
        lseek(fd, -i, SEEK_END);
        load_file(fd, (char *) &namelen, sizeof(namelen));

        if (namelen > MAX_FUNC_NAME_SIZE) {
            LERR("open namelen fail %s %d", srcfile, namelen);
            close(fd);
            return -1;
        }

        char str[MAX_FUNC_NAME_SIZE + 1];
        str[namelen] = 0;

        i += namelen;
        lseek(fd, -i, SEEK_END);
        load_file(fd, str, namelen);

        gString2Id[str] = id;
        gId2String[id] = str;

        LLOG("name %d %s", id, str);
        namenum++;
    }

    gLoadCallStack.clear();
    while (i < cnt) {
        CallStack cs;

        i += sizeof(cs);
        lseek(fd, -i, SEEK_END);
        load_file(fd, (char *) &cs, sizeof(cs));

        gLoadCallStack.push_back(cs);

        LLOG("CallStack %d %d %s", cs.depth, cs.count, gId2String[cs.stack[cs.depth - 1]].c_str());
    }

    LLOG("load ok %d %d", gString2Id.size(), gLoadCallStack.size());

    close(fd);
    return 0;
}

static int output_text(const char *dstfile) {

    int fd = open(dstfile, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        LERR("open dstfile fail %s", dstfile);
        return -1;
    }

    int total = 0;

    std::unordered_map<int, int> funcmap;
    for (auto iter = gLoadCallStack.begin(); iter != gLoadCallStack.end(); iter++) {
        const CallStack &cs = *iter;

        funcmap[cs.stack[cs.depth - 1]] += cs.count;
        total += cs.count;
    }

    std::vector <std::pair<int, int>> funcarr;
    for (auto iter = funcmap.begin(); iter != funcmap.end(); iter++) {
        funcarr.push_back(std::make_pair(iter->first, iter->second));
    }

    std::sort(funcarr.begin(), funcarr.end(), [](const std::pair<int, int> &l, const std::pair<int, int> &r) {
                  return r.second < l.second;
              }
    );

    std::stringstream ss;
    for (auto iter = funcarr.begin(); iter != funcarr.end(); iter++) {
        ss << iter->second * 100 / total << "%\t" << gId2String[iter->first] << "\n";
    }

    LLOG("%s", ss.str().c_str())

    flush_file(fd, ss.str().c_str(), ss.str().length());

    close(fd);

    return 0;
}

static int ltext(lua_State *L) {

    const char *srcfile = lua_tostring(L, 1);
    const char *dstfile = lua_tostring(L, 2);

    int ret = load(srcfile);
    if (ret != 0) {
        LERR("load fail %d %s %s", ret, srcfile, dstfile);
        lua_pushinteger(L, ret);
        return 1;
    }

    ret = output_text(dstfile);
    if (ret != 0) {
        LERR("output_text fail %d %s %s", ret, srcfile, dstfile);
        lua_pushinteger(L, ret);
        return 1;
    }

    lua_pushinteger(L, ret);
    return 1;
}

static int output_dot(const char *dstfile) {

    int fd = open(dstfile, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        LERR("open dstfile fail %s", dstfile);
        return -1;
    }

    int totalself = 0;
    std::unordered_map<int, int> funcmapself;
    for (auto iter = gLoadCallStack.begin(); iter != gLoadCallStack.end(); iter++) {
        const CallStack &cs = *iter;

        funcmapself[cs.stack[cs.depth - 1]] += cs.count;
        totalself += cs.count;
    }

    int total = 0;
    std::unordered_map<int, int> funcmap;
    for (auto iter = gLoadCallStack.begin(); iter != gLoadCallStack.end(); iter++) {
        const CallStack &cs = *iter;

        for (int i = 0; i < cs.depth; i++) {
            funcmap[cs.stack[i]] += cs.count;
        }
        total += cs.count;
    }

    std::vector <std::pair<int, int>> funcarr;
    for (auto iter = funcmap.begin(); iter != funcmap.end(); iter++) {
        funcarr.push_back(std::make_pair(iter->first, iter->second));
    }

    std::sort(funcarr.begin(), funcarr.end(), [](const std::pair<int, int> &l, const std::pair<int, int> &r) {
                  return r.second < l.second;
              }
    );

    struct pair_hash {
        std::size_t operator()(const std::pair<int, int> &p) const {
            return ((size_t) p.first << 32) + p.second;
        }
    };

    std::set<int> hassonset;
    std::unordered_set <std::pair<int, int>, pair_hash> funccallset;
    for (auto iter = gLoadCallStack.begin(); iter != gLoadCallStack.end(); iter++) {
        const CallStack &cs = *iter;

        for (int i = 0; i < cs.depth - 1; i++) {
            funccallset.insert(std::make_pair(cs.stack[i], cs.stack[i + 1]));
            hassonset.insert(cs.stack[i]);
        }
    }

    std::stringstream ss;
    ss << "digraph G {\n";

    for (auto iter = funcarr.begin(); iter != funcarr.end(); iter++) {
        ss << "\tnode" << iter->first
           << " [label=\"" << gId2String[iter->first] << "\\r"
           << funcmapself[iter->first] << " (" << funcmapself[iter->first] * 100 / totalself << "%)" << "\\r";

        if (hassonset.find(iter->first) != hassonset.end()) {
            ss << iter->second << " (" << iter->second * 100 / total << "%)" << "\\r";
        }

        int fontsize = funcmapself[iter->first] * 100 / totalself;
        if (fontsize < 10) {
            fontsize = 10;
        }

        ss << "\";"
           << "fontsize=" << fontsize
           << ";shape=box;"
           << "];\n";
    }

    for (auto iter = funccallset.begin(); iter != funccallset.end(); iter++) {
        const std::pair<int, int> &cp = *iter;
        float linewidth = funcmap[cp.second] * 3.f / total;
        if (linewidth < 0.5f) {
            linewidth = 0.5f;
        }
        ss << "\tnode" << cp.first << "->" << "node" << cp.second
           << " [style=\"setlinewidth(" << linewidth << ")\""
           << " label=" << funcmap[cp.second]
           << "];\n";
    }

    ss << "}\n";

    LLOG("%s", ss.str().c_str())

    flush_file(fd, ss.str().c_str(), ss.str().length());

    close(fd);

    return 0;
}

static int ldot(lua_State *L) {

    const char *srcfile = lua_tostring(L, 1);
    const char *dstfile = lua_tostring(L, 2);

    int ret = load(srcfile);
    if (ret != 0) {
        LERR("load fail %d %s %s", ret, srcfile, dstfile);
        lua_pushinteger(L, ret);
        return 1;
    }

    ret = output_dot(dstfile);
    if (ret != 0) {
        LERR("output_dot fail %d %s %s", ret, srcfile, dstfile);
        lua_pushinteger(L, ret);
        return 1;
    }

    lua_pushinteger(L, ret);
    return 1;
}

static int lsvg(lua_State *L) {

    const char *srcfile = lua_tostring(L, 1);
    const char *dstfile = lua_tostring(L, 2);

    int ret = load(srcfile);
    if (ret != 0) {
        LERR("load fail %d %s %s", ret, srcfile, dstfile);
        lua_pushinteger(L, ret);
        return 1;
    }

    std::string tmpdot = dstfile;
    tmpdot += ".dot";
    ret = output_dot(tmpdot.c_str());
    if (ret != 0) {
        LERR("output_dot fail %d %s %s", ret, srcfile, tmpdot.c_str());
        lua_pushinteger(L, ret);
        return 1;
    }

    GVC_t *gvc;
    Agraph_t *g;
    FILE *fp;
    gvc = gvContext();
    fp = fopen(tmpdot.c_str(), "r");
    g = agread(fp, 0);
    gvLayout(gvc, g, "dot");
    gvRender(gvc, g, "svg", fopen(dstfile, "w"));
    gvFreeLayout(gvc, g);
    agclose(g);
    gvFreeContext(gvc);

    lua_pushinteger(L, ret);
    return 1;

}

extern "C" int luaopen_libplua(lua_State *L) {
    luaL_checkversion(L);
    luaL_Reg l[] = {
            {"start", lstart},
            {"stop",  lstop},
            {"text",  ltext},
            {"dot",   ldot},
            {"svg",   lsvg},
            {NULL,    NULL},
    };
    luaL_newlib(L, l);
    return 1;
}
