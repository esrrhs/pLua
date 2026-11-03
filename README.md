# pLua

[English](README.md) | [中文文档](README_CN.md)

[<img src="https://img.shields.io/github/license/esrrhs/pLua">](https://github.com/esrrhs/pLua)
[<img src="https://img.shields.io/github/languages/top/esrrhs/pLua">](https://github.com/esrrhs/pLua)
[<img src="https://img.shields.io/github/actions/workflow/status/esrrhs/pLua/ccpp.yml?branch=master">](https://github.com/esrrhs/pLua/actions)

Lua Performance Profiler & Memory Profiler.

## Overview
Similar to `gperftools`, **pLua** profiles CPU hotspots and memory allocations for Lua programs.

## Features
- **Easy to Use**: Only a few lines of code to start profiling, or inject into running processes via [hookso](https://github.com/esrrhs/hookso) without modifying code.
- **Accurate**: Uses periodic timer sampling (`ITIMER_PROF`) rather than Lua line hooks to capture execution hotspots accurately and exclude sleep/idle time.
- **Lightweight**: Sampling approach minimizes runtime overhead on the host process.
- **Intuitive**: Outputs call graphs, compatible with `pprof` (`gperftools`), and generates [FlameGraph](https://github.com/brendangregg/FlameGraph) SVGs.

---

## Dependencies & Installation

pLua requires Lua development headers and libraries (Lua 5.1/5.2/5.3/5.4 or LuaJIT). Please install the Lua dependency package using your system package manager:

### Ubuntu / Debian
```shell
sudo apt-get update
sudo apt-get install -y lua5.3 liblua5.3-dev
```

### CentOS / RHEL / Fedora
```shell
# CentOS / RHEL (EPEL)
sudo yum install -y lua-devel
```

### macOS (Homebrew)
```shell
brew install lua
```

---

## Build

Compile using CMake:

```shell
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

Compiled library will be placed in `bin/libplua.so`. If Go is available, the `plua` profile converter tool will also be built automatically into `bin/plua`.

---

## Unit Testing

Run unit tests via CTest:

```shell
cd build
ctest --output-on-failure
```

Or run the Lua unit test directly:

```shell
cd test
lua test_unit.lua
```

---

## Usage

### 1. Collect CPU Profile Data

#### Option A: Embed in Lua Code
```lua
-- Load libplua.so
local p = require "libplua"

-- Start CPU profiling
-- Arg 1: sampling duration in seconds (0 = profile until stopped)
-- Arg 2: output profile file path
p.start(0, "call.pro")

do_some_thing()

-- Stop profiling and write out profile file
p.stop()
```

#### Option B: Inject via [hookso](https://github.com/esrrhs/hookso)
```shell
# a) Retrieve lua_State pointer from the target process (e.g. from lua_settop argument)
./hookso arg $PID xxx.so lua_settop 1 
123456

# b) Load libplua.so into the target process
./hookso dlopen $PID ./libplua.so

# c) Call lrealstart to manually start profiling: lrealstart(L, 0, "./call.pro")
./hookso call $PID libplua.so lrealstart i=123456 i=0 s="./call.pro"

# d) Call lrealstop to stop profiling
./hookso call $PID libplua.so lrealstop i=123456
```

---

### 2. Collect Memory Profile Data

#### Option A: Embed in Lua Code
```lua
-- Load libplua.so
local p = require "libplua"

-- Start memory profiling
-- Arg 1: sampling duration in seconds (0 = profile until stopped)
-- Arg 2: prefix for output files (will generate <prefix>_ALLOC_SIZE.pro and <prefix>_USAGE.pro)
p.start_mem(0, "mem.pro")

do_some_thing()

-- Optional: trigger GC before stopping if analyzing retained memory
collectgarbage("collect")

-- Stop profiling and output result files
p.stop_mem()
```

#### Option B: Inject via [hookso](https://github.com/esrrhs/hookso)
```shell
# a) Retrieve lua_State pointer
./hookso arg $PID xxx.so lua_settop 1 
123456

# b) Load libplua.so
./hookso dlopen $PID ./libplua.so

# c) Call lrealstartmem: lrealstartmem(L, 0, "./mem.pro")
./hookso call $PID libplua.so lrealstartmem i=123456 i=0 s="./mem.pro"

# d) Call lrealstopmem to stop
./hookso call $PID libplua.so lrealstopmem i=123456
```

---

## Visualization & Tools

The `tools/` directory provides utilities to convert `.pro` profile data into FlameGraph SVGs and call graph images.

### Prerequisites (Optional)
- **FlameGraph**: `show.sh` will automatically fetch `flamegraph.pl` from GitHub if not found locally or in `PATH`. On CentOS/RHEL, you may need `yum install perl-open`.
- **Graphviz** (for call graph PNG): `sudo apt install graphviz` / `sudo yum install graphviz`
- **pprof** (for call graph DOT/PNG): `go install github.com/google/pprof@latest` (Ubuntu ≤24.04: `sudo apt install google-perftools`; RHEL: `sudo yum install gperftools`)

### Generate Visualizations
```shell
cd tools
./show.sh ../test
```

This generates:
- `<name>.fl`: Folded stack traces
- `<name>.svg`: Interactive SVG flame graph
- `<name>.prof`: Symbolized pprof profile
- `<name>.dot`: Graphviz call graph
- `<name>.png`: Rendered PNG call graph (if graphviz and pprof are installed)

---

## Examples

### CPU FlameGraph & Call Graph
Generated from `test/test_cpu.lua`:

![call](test/call.png)

### Memory Allocation Graph
Generated from `test/test_mem.lua`:

![mem_ALLOC_SIZE](test/mem_ALLOC_SIZE.png)

---

## Related Projects
- [lua-family-bucket](https://github.com/esrrhs/lua-family-bucket)
