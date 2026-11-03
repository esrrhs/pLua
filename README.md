# pLua

[English](README.md) | [中文文档](README_CN.md)

[<img src="https://img.shields.io/github/license/esrrhs/pLua">](https://github.com/esrrhs/pLua)
[<img src="https://img.shields.io/github/languages/top/esrrhs/pLua">](https://github.com/esrrhs/pLua)
[<img src="https://img.shields.io/github/actions/workflow/status/esrrhs/pLua/ccpp.yml?branch=master">](https://github.com/esrrhs/pLua/actions)

Lua Performance Profiler & Memory Profiler.

## Overview

Similar to `gperftools`, **pLua** profiles CPU hotspots and memory allocations for Lua programs. Embed it as a shared library, collect `.pro` profiles, then convert them with `tools/plua` for [FlameGraph](https://github.com/brendangregg/FlameGraph) SVGs or `pprof`-compatible call graphs.

> Chinese documentation: [README_CN.md](README_CN.md)

## Features

- **Easy to use**: A few lines of Lua to start/stop profiling. On **Linux** you can also inject into a running process via [hookso](https://github.com/esrrhs/hookso) without modifying code.
- **Accurate**: On POSIX, default CPU backend uses periodic timer sampling (`ITIMER_PROF` / `SIGPROF`), capturing execution hotspots and excluding sleep/idle time. Falls back to `LUA_MASKCOUNT` on Windows, or when forced via config/env.
- **Multi-state**: Each `lua_State` has its own profiler session. You can profile several states in one process. **Multi-state is not multi-OS-thread safe** — do not call pLua from multiple OS threads concurrently without external synchronization.
- **Independent CPU / Memory**: CPU and memory profiling use separate running flags; both can run at the same time on one state.
- **Configurable rates**: `set_config` / `get_config`, environment variables, and optional rate arguments on `start` / `start_mem`.
- **Runtime status**: `status()` reports whether CPU/mem are running, sample counts, output paths, backend, and a config snapshot. `close()` tears down the session for the current `lua_State`.
- **Lightweight**: Sampling approach keeps runtime overhead low.
- **Intuitive**: Outputs call graphs compatible with `pprof` (`gperftools`) and FlameGraph collapsed stacks / SVGs.

---

## Platforms

| Platform | Library artifact | CPU backend | hookso injection | Notes |
|----------|------------------|-------------|------------------|-------|
| Linux    | `bin/libplua.so` | `SIGPROF` (default) or `LUA_MASKCOUNT` | Supported | Primary platform |
| macOS    | `bin/libplua.so` (also loadable as `.dylib` via cpath) | `SIGPROF` (default) or count | **Not supported** | Embedding OK |
| Windows  | `bin/libplua.dll` | `LUA_MASKCOUNT` | **Not supported** | Embedding OK |

### Lua version support

pLua targets **Lua 5.2 / 5.3 / 5.4** (uses `luaL_newlib` / `luaL_checkversion` / `luaL_requiref`).

**Lua 5.1 and LuaJIT are not supported** by the current module loader API.

---

## Dependencies & Installation

### Ubuntu / Debian

```shell
sudo apt-get update
sudo apt-get install -y lua5.3 liblua5.3-dev
```

### CentOS / RHEL / Fedora

```shell
sudo yum install -y lua-devel
```

### macOS (Homebrew)

```shell
brew install lua
```

### Windows

Install Lua 5.4 development headers/libs (vcpkg, Chocolatey, or a manual build), then point CMake at them via `LUA_INCLUDE_DIR` / `LUA_LIBRARIES` if needed.

---

## Build

```shell
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

Artifacts land in `bin/`:

- Linux / macOS: `libplua.so`
- Windows: `libplua.dll`
- If Go is available: `plua` profile converter (`tools/plua.go`)

---

## Unit Testing

```shell
cd build
ctest --output-on-failure
```

Or run the Lua unit test directly (after build):

```shell
cd test
lua test_unit.lua
```

Multi-`lua_State` coverage is in the C++ harness (`unit_test` / `unit_test --multistate`).

---

## Usage

### Module name

```lua
local p = require "libplua"   -- preferred (matches libplua.so / .dll)
-- If you ship a copy/symlink named plua.so, require "plua" also works (luaopen_plua).
```

Add the library directory to `package.cpath` as needed:

```lua
package.cpath = "./bin/?.so;./bin/?.dylib;./bin/?.dll;" .. package.cpath
```

### 1. Collect CPU profile data

#### Embed in Lua code

```lua
local p = require "libplua"

-- Arg 1: sampling duration in seconds (0 = until stopped)
-- Arg 2: output profile file path
-- Arg 3 (optional): CPU sample interval in milliseconds
--                   (updates global default; else PLUA_CPU_INTERVAL_MS / 10)
p.start(0, "call.pro")
-- p.start(0, "call.pro", 5)

do_some_thing()

p.stop()
```

#### Inject via [hookso](https://github.com/esrrhs/hookso) (**Linux only**)

```shell
# a) Obtain a lua_State* in the target process
./hookso arg $PID xxx.so lua_settop 1
# -> prints L, e.g. 123456

# b) Load libplua.so
./hookso dlopen $PID ./libplua.so

# c) Start CPU profiling: lrealstart(L, seconds, path)
./hookso call $PID libplua.so lrealstart i=123456 i=0 s="./call.pro"

# d) Stop CPU profiling
./hookso call $PID libplua.so lrealstop i=123456
```

hookso injection is **not supported on macOS or Windows** — use embedding there.

---

### 2. Collect memory profile data

#### Embed in Lua code

```lua
local p = require "libplua"

-- Arg 1: max samples (0 = until stopped)
-- Arg 2: output prefix -> <prefix>_ALLOC_SIZE.pro and <prefix>_USAGE.pro
-- Arg 3 (optional): sampling rate in bytes
--                   (updates global default; else PLUA_MEM_RATE / 524288)
p.start_mem(0, "mem.pro")
-- p.start_mem(0, "mem.pro", 262144)

do_some_thing()
collectgarbage("collect")  -- optional, for retained-memory (_USAGE) view
p.stop_mem()
```

#### Inject via hookso (**Linux only**)

```shell
./hookso arg $PID xxx.so lua_settop 1
./hookso dlopen $PID ./libplua.so
./hookso call $PID libplua.so lrealstartmem i=123456 i=0 s="./mem.pro"
./hookso call $PID libplua.so lrealstopmem i=123456
```

---

### 3. Multi `lua_State`

Each state keeps an independent session (CPU and/or mem). Stopping one state does not stop others.

```lua
-- In host C/C++: create L1, L2, require libplua in each, call start/stop per state.
```

**Thread safety:** pLua is safe for multiple `lua_State` values used from **one OS thread** (or with external synchronization). It is **not** safe to drive two states from two OS threads without external locking.

---

### 4. Hook exclusivity & coroutine boundaries

**`lua_sethook` exclusivity.** pLua installs its own debug hook via `lua_sethook` for CPU sampling (and briefly when arming mem samples). It expects **exclusive** ownership of the hook on that `lua_State` while profiling:

- Do **not** run another debugger / profiler / coverage tool that also calls `lua_sethook` on the same state at the same time — they will overwrite each other and produce wrong or missing samples (or break the other tool).
- Stop pLua (`stop` / `stop_mem` / `close`) before attaching a different hook-based tool.

**Coroutines.** `coroutine.create` (and related APIs) produce a **new** Lua thread that shares the global state but **does not inherit** the parent's debug hook. Child coroutines are **not** profiled automatically:

- Call `start` / `start_mem` on each coroutine/`lua_State` you care about, **or**
- Only interpret profiles as covering the thread(s) where you explicitly started pLua.

Session cleanup: stopping both CPU and mem (or calling `close`) unregisters the session; `lua_close` also drops it via a registry userdata `__gc` (no dangling `gSessions` entry).

---

### 5. Runtime status

```lua
local st = p.status()
-- st.cpu_running / st.mem_running  (boolean)
-- st.cpu_samples / st.mem_samples  (number, samples collected so far)
-- st.cpu_output / st.mem_output    (path or nil)
-- st.cpu_backend                   ("sigprof" or "count")
-- st.config                        (same shape as get_config())

p.close()  -- stop cpu+mem if needed and free the session for this state
```

On start failure, `start` / `start_mem` return `-1` plus an error string as the second result (e.g. already running, missing path).

---

### 6. Configurable sample rates

Lua API:

```lua
p.set_config({
  cpu_interval_ms = 10,   -- SIGPROF interval (POSIX)
  cpu_hook_count = 1000,  -- LUA_MASKCOUNT step (Windows / fallback)
  mem_rate = 524288,      -- bytes between mem samples
  cpu_backend = "sigprof" -- or "count" / "maskcount"
})

local cfg = p.get_config()
print(cfg.cpu_backend, cfg.cpu_interval_ms, cfg.cpu_hook_count, cfg.mem_rate)
```

Environment variables (read once at first use of start / config APIs):

| Variable | Meaning | Default |
|----------|---------|---------|
| `PLUA_CPU_INTERVAL_MS` | POSIX timer interval (ms) | `10` |
| `PLUA_CPU_HOOK_COUNT` | `LUA_MASKCOUNT` instructions per sample | `1000` |
| `PLUA_MEM_RATE` | Memory sampling period (bytes) | `524288` |
| `PLUA_CPU_BACKEND` | `sigprof` or `count` / `maskcount` | `sigprof` on POSIX, always `count` on Windows |

---

## `.pro` file format (v1)

Versioned profiles start with:

- magic `PLUA` (4 bytes, little-endian `0x41554C50`)
- version `uint32` LE (currently `1`)
- then the legacy payload (call stacks + trailing string table)

`tools/plua` accepts both v1 (with header) and legacy files (no header, treated as version 0).

---

## Visualization & Tools

### `tools/plua`

Go converter (built to `bin/plua` when Go is available):

```shell
# FlameGraph collapsed stacks
./bin/plua -i call.pro -flame call.fl

# pprof symbolized profile
./bin/plua -i call.pro -pprof call.prof

# Both
./bin/plua -i call.pro -flame call.fl -pprof call.prof
```

`-collapsed` is an alias for `-flame`.

### Prerequisites (optional)

- **FlameGraph**: `show.sh` fetches `flamegraph.pl` if needed
- **Graphviz**: for call graph PNG (`dot`)
- **pprof**: `go install github.com/google/pprof@latest`

### Batch generate visualizations

```shell
cd tools
./show.sh ../test
```

Outputs per `.pro`: `.fl` (collapsed), `.svg` (flamegraph), `.prof` (pprof), and optionally `.dot` / `.png` call graphs.

---

## Examples

### CPU FlameGraph & Call Graph

![call](test/call.png)

### Memory Allocation Graph

![mem_ALLOC_SIZE](test/mem_ALLOC_SIZE.png)

---

## Related Projects

- [lua-family-bucket](https://github.com/esrrhs/lua-family-bucket)
- [hookso](https://github.com/esrrhs/hookso) (Linux process injection helper)
