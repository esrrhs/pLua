# pLua

[English](README.md) | [中文文档](README_CN.md)

[<img src="https://img.shields.io/github/license/esrrhs/pLua">](https://github.com/esrrhs/pLua)
[<img src="https://img.shields.io/github/languages/top/esrrhs/pLua">](https://github.com/esrrhs/pLua)
[<img src="https://img.shields.io/github/actions/workflow/status/esrrhs/pLua/ccpp.yml?branch=master">](https://github.com/esrrhs/pLua/actions)

Lua Performance Profiler & Memory Profiler.

## Overview
Similar to `gperftools`, **pLua** profiles CPU hotspots and memory allocations for Lua programs.

## Features
- **Easy to Use**: Only a few lines of code to start profiling. On Linux you can also inject into running processes via [hookso](https://github.com/esrrhs/hookso) without modifying code.
- **Accurate**: On POSIX, uses periodic timer sampling (`ITIMER_PROF` / `SIGPROF`) rather than only Lua line hooks, capturing execution hotspots and excluding sleep/idle time. On Windows (and optionally elsewhere) falls back to `LUA_MASKCOUNT`-based sampling.
- **Multi-state**: Each `lua_State` has its own profiler session. You can profile several states in one process. **Multi-state is not multi-OS-thread safe** — do not call pLua from multiple OS threads concurrently.
- **Independent CPU / Memory**: CPU and memory profiling use separate running flags; they can run at the same time on one state.
- **Lightweight**: Sampling approach minimizes runtime overhead on the host process.
- **Intuitive**: Outputs call graphs, compatible with `pprof` (`gperftools`), and generates [FlameGraph](https://github.com/brendangregg/FlameGraph) SVGs.

---

## Platforms

| Platform | Library artifact | CPU backend | hookso injection | Notes |
|----------|------------------|-------------|------------------|-------|
| Linux    | `bin/libplua.so` | `SIGPROF` (default) or `LUA_MASKCOUNT` | Supported | Primary platform |
| macOS    | `bin/libplua.so` (also loadable as `.dylib` via cpath) | `SIGPROF` (default) or count | **Not supported** | Embedding OK |
| Windows  | `bin/libplua.dll` | `LUA_MASKCOUNT` | **Not supported** | Embedding OK |

### Lua version support
pLua targets **Lua 5.2 / 5.3 / 5.4** (uses `luaL_newlib` / `luaL_checkversion` / `luaL_requiref`).

**Lua 5.1 and LuaJIT are not supported** by the current module loader API. Older README claims of 5.1/JIT support were inaccurate.

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
- Linux/macOS: `libplua.so`
- Windows: `libplua.dll`
- If Go is available: `plua` profile converter

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
local p = require "libplua"   -- preferred (matches libplua.so/.dll)
-- If you ship a copy/symlink named plua.so, require "plua" also works (luaopen_plua).
```

Add the library directory to `package.cpath` as needed:
```lua
package.cpath = "./bin/?.so;./bin/?.dylib;./bin/?.dll;" .. package.cpath
```

### 1. Collect CPU Profile Data

#### Embed in Lua Code
```lua
local p = require "libplua"

-- Arg 1: sampling duration in seconds (0 = until stopped)
-- Arg 2: output profile file path
-- Arg 3 (optional): CPU sample interval in milliseconds (default 10, or PLUA_CPU_INTERVAL_MS)
p.start(0, "call.pro")

do_some_thing()

p.stop()
```

#### Inject via [hookso](https://github.com/esrrhs/hookso) (Linux only)
```shell
./hookso arg $PID xxx.so lua_settop 1
./hookso dlopen $PID ./libplua.so
./hookso call $PID libplua.so lrealstart i=<L> i=0 s="./call.pro"
./hookso call $PID libplua.so lrealstop i=<L>
```
hookso injection is **not supported on macOS or Windows**.

---

### 2. Collect Memory Profile Data

```lua
local p = require "libplua"

-- Arg 1: max samples (0 = until stopped)
-- Arg 2: output prefix -> <prefix>_ALLOC_SIZE.pro and <prefix>_USAGE.pro
-- Arg 3 (optional): sampling rate in bytes (default 524288 / 512KiB, or PLUA_MEM_RATE)
p.start_mem(0, "mem.pro")

do_some_thing()
collectgarbage("collect")  -- optional, for retained-memory view
p.stop_mem()
```

---

### 3. Multi lua_State

Each state keeps an independent session (CPU and/or mem). Stopping one state does not stop others.

```lua
-- In host C/C++: create L1, L2, require libplua in each, call start/stop per state.
```

**Thread safety:** pLua is safe for multiple `lua_State` values used from **one OS thread** (or with external synchronization). It is **not** safe to drive two states from two OS threads without external locking.

---

### 4. Configurable sample rates

Lua API:
```lua
p.set_config({
  cpu_interval_ms = 10,   -- SIGPROF interval (POSIX)
  cpu_hook_count = 1000,  -- LUA_MASKCOUNT step (Windows / fallback)
  mem_rate = 524288,      -- bytes between mem samples
  cpu_backend = "sigprof" -- or "count" / "maskcount"
})
print(p.get_config().cpu_backend)
```

Environment variables (read once at first use):
| Variable | Meaning | Default |
|----------|---------|---------|
| `PLUA_CPU_INTERVAL_MS` | POSIX timer interval (ms) | `10` |
| `PLUA_CPU_HOOK_COUNT` | `LUA_MASKCOUNT` instructions per sample | `1000` |
| `PLUA_MEM_RATE` | Memory sampling period (bytes) | `524288` |
| `PLUA_CPU_BACKEND` | `sigprof` or `count` | `sigprof` on POSIX, `count` on Windows |

---

## .pro file format

Versioned profiles start with:
- magic `PLUA` (4 bytes, little-endian `0x41554C50`)
- version `uint32` LE (currently `1`)
- then the legacy payload (call stacks + trailing string table)

`tools/plua` accepts both v1 (with header) and legacy files (no header, treated as version 0).

---

## Visualization & Tools

### Prerequisites (Optional)
- **FlameGraph**: `show.sh` fetches `flamegraph.pl` if needed
- **Graphviz**: for call graph PNG
- **pprof**: `go install github.com/google/pprof@latest`

### Generate Visualizations
```shell
cd tools
./show.sh ../test
```

---

## Examples

### CPU FlameGraph & Call Graph
![call](test/call.png)

### Memory Allocation Graph
![mem_ALLOC_SIZE](test/mem_ALLOC_SIZE.png)

---

## Related Projects
- [lua-family-bucket](https://github.com/esrrhs/lua-family-bucket)
