# pLua

[English](README.md) | [中文文档](README_CN.md)

[<img src="https://img.shields.io/github/license/esrrhs/pLua">](https://github.com/esrrhs/pLua)
[<img src="https://img.shields.io/github/languages/top/esrrhs/pLua">](https://github.com/esrrhs/pLua)
[<img src="https://img.shields.io/github/actions/workflow/status/esrrhs/pLua/ccpp.yml?branch=master">](https://github.com/esrrhs/pLua/actions)

Lua 性能及内存分析工具。

## 简介

类似于 `gperftools`，**pLua** 可以分析 Lua 程序的 CPU 热点与内存分配。以共享库方式嵌入，采集 `.pro` 采样文件，再用 `tools/plua` 转换为 [火焰图](https://github.com/brendangregg/FlameGraph) SVG 或兼容 `pprof` 的调用图。

> 主文档（English）：[README.md](README.md)

## 特性

- **简单**：只需几行 Lua 即可启停采样。在 **Linux** 上还可通过 [hookso](https://github.com/esrrhs/hookso) 动态注入运行中的进程，无需改业务代码。
- **准确**：POSIX 默认使用周期性定时采样（`ITIMER_PROF` / `SIGPROF`），更能捕获执行热点，并自动剔除 sleep 等空闲时间。Windows（或通过配置强制）回退到 `LUA_MASKCOUNT`。
- **多 State**：每个 `lua_State` 拥有独立会话，可在同一进程中对多个 State 分别采样。**多 State ≠ 多 OS 线程安全**——不要在未加同步的情况下从多个 OS 线程并发调用 pLua。
- **CPU / 内存独立开关**：`cpuRunning` / `memRunning` 分离，同一 State 上可同时开启。
- **可配置采样率**：`set_config` / `get_config`、环境变量，以及 `start` / `start_mem` 的可选速率参数。
- **轻量**：基于采样，对宿主性能影响小。
- **直观**：兼容 `pprof`（`gperftools`），可生成折叠堆栈与火焰图 SVG。

---

## 平台

| 平台 | 产物 | CPU 后端 | hookso 注入 | 说明 |
|------|------|----------|-------------|------|
| Linux | `bin/libplua.so` | `SIGPROF`（默认）或 `LUA_MASKCOUNT` | 支持 | 主平台 |
| macOS | `bin/libplua.so`（也可通过 cpath 以 `.dylib` 加载） | `SIGPROF`（默认）或 count | **不支持** | 嵌入可用 |
| Windows | `bin/libplua.dll` | `LUA_MASKCOUNT` | **不支持** | 嵌入可用 |

### Lua 版本

支持 **Lua 5.2 / 5.3 / 5.4**（依赖 `luaL_newlib` / `luaL_checkversion` / `luaL_requiref`）。

当前模块加载 API **不支持 Lua 5.1 / LuaJIT**（旧文档中的兼容声明不准确）。

---

## 依赖安装

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

安装 Lua 5.4 开发头文件与库（vcpkg、Chocolatey 或自行编译），必要时通过 `LUA_INCLUDE_DIR` / `LUA_LIBRARIES` 告知 CMake。

---

## 编译构建

```shell
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

产物位于 `bin/`：

- Linux / macOS：`libplua.so`
- Windows：`libplua.dll`
- 若已安装 Go：会额外编译 `plua` 转换工具（`tools/plua.go`）

---

## 单元测试

```shell
cd build
ctest --output-on-failure
```

或直接运行 Lua 单元测试（需先完成构建）：

```shell
cd test
lua test_unit.lua
```

多 `lua_State` 场景由 C++ 测试覆盖（`unit_test` / `unit_test --multistate`）。

---

## 使用说明

### 模块名

```lua
local p = require "libplua"   -- 推荐（与 libplua.so / .dll 对应）
-- 若将库命名/软链为 plua.so，也可 require "plua"（提供 luaopen_plua 别名）
```

按需把库目录加入 `package.cpath`：

```lua
package.cpath = "./bin/?.so;./bin/?.dylib;./bin/?.dll;" .. package.cpath
```

### 1. 获取 CPU 采样数据

#### 方式一：在 Lua 中嵌入

```lua
local p = require "libplua"

-- 参数 1：采样时长（秒），0 表示一直采样直到显式停止
-- 参数 2：输出 .pro 文件路径
-- 参数 3（可选）：CPU 采样间隔毫秒（会更新全局默认；否则用 PLUA_CPU_INTERVAL_MS / 10）
p.start(0, "call.pro")
-- p.start(0, "call.pro", 5)

do_some_thing()

p.stop()
```

#### 方式二：使用 [hookso](https://github.com/esrrhs/hookso) 注入（**仅 Linux**）

```shell
# a) 获取目标进程中的 lua_State 指针
./hookso arg $PID xxx.so lua_settop 1
# -> 例如输出 123456

# b) 加载 libplua.so
./hookso dlopen $PID ./libplua.so

# c) 开启 CPU 采样：lrealstart(L, seconds, path)
./hookso call $PID libplua.so lrealstart i=123456 i=0 s="./call.pro"

# d) 结束 CPU 采样
./hookso call $PID libplua.so lrealstop i=123456
```

macOS / Windows **不支持** hookso 注入，请使用嵌入方式。

---

### 2. 获取内存采样数据

#### 方式一：在 Lua 中嵌入

```lua
local p = require "libplua"

-- 参数 1：最大采样次数（0 表示一直采样直到停止）
-- 参数 2：输出文件名前缀，生成 <prefix>_ALLOC_SIZE.pro 与 <prefix>_USAGE.pro
-- 参数 3（可选）：采样字节率（会更新全局默认；否则用 PLUA_MEM_RATE / 524288）
p.start_mem(0, "mem.pro")
-- p.start_mem(0, "mem.pro", 262144)

do_some_thing()
collectgarbage("collect")  -- 可选，便于观察常驻内存（_USAGE）
p.stop_mem()
```

#### 方式二：使用 hookso 注入（**仅 Linux**）

```shell
./hookso arg $PID xxx.so lua_settop 1
./hookso dlopen $PID ./libplua.so
./hookso call $PID libplua.so lrealstartmem i=123456 i=0 s="./mem.pro"
./hookso call $PID libplua.so lrealstopmem i=123456
```

---

### 3. 多 `lua_State`

每个 State 维护独立会话（CPU 与/或内存）。停止其中一个不会影响其他 State。

```lua
-- 在宿主 C/C++ 中：创建 L1、L2，分别 require libplua，再按 State 调用 start/stop。
```

**线程安全：** 多个 `lua_State` 从 **同一 OS 线程** 使用是安全的（或自行加锁）。**不要**在没有外部同步的情况下从两个 OS 线程分别驱动两个 State。

---

### 4. 可配置采样率

Lua API：

```lua
p.set_config({
  cpu_interval_ms = 10,   -- SIGPROF 间隔（POSIX）
  cpu_hook_count = 1000,  -- LUA_MASKCOUNT 步长（Windows / 回退）
  mem_rate = 524288,      -- 内存采样字节周期
  cpu_backend = "sigprof" -- 或 "count" / "maskcount"
})

local cfg = p.get_config()
print(cfg.cpu_backend, cfg.cpu_interval_ms, cfg.cpu_hook_count, cfg.mem_rate)
```

环境变量（在首次调用 start / config API 时读取一次）：

| 变量 | 含义 | 默认 |
|------|------|------|
| `PLUA_CPU_INTERVAL_MS` | POSIX 定时器间隔（毫秒） | `10` |
| `PLUA_CPU_HOOK_COUNT` | `LUA_MASKCOUNT` 每样本指令数 | `1000` |
| `PLUA_MEM_RATE` | 内存采样周期（字节） | `524288` |
| `PLUA_CPU_BACKEND` | `sigprof` 或 `count` / `maskcount` | POSIX 默认 `sigprof`，Windows 固定为 `count` |

---

## `.pro` 文件格式（v1）

带版本的采样文件以如下头部开头：

- magic `PLUA`（4 字节，小端 `0x41554C50`）
- version `uint32` LE（当前为 `1`）
- 随后为兼容旧版的负载（调用栈 + 尾部字符串表）

`tools/plua` 同时接受 v1（有头）与旧格式（无头，视为 version 0）。

---

## 可视化与工具

### `tools/plua`

Go 转换工具（安装了 Go 时会编译到 `bin/plua`）：

```shell
# 火焰图折叠堆栈
./bin/plua -i call.pro -flame call.fl

# pprof 符号化 profile
./bin/plua -i call.pro -pprof call.prof

# 同时生成
./bin/plua -i call.pro -flame call.fl -pprof call.prof
```

`-collapsed` 是 `-flame` 的别名。

### 可选依赖

- **FlameGraph**：若本地没有 `flamegraph.pl`，`show.sh` 会尝试自动下载
- **Graphviz**：用于调用图 PNG（`dot`）
- **pprof**：`go install github.com/google/pprof@latest`

### 批量生成可视化

```shell
cd tools
./show.sh ../test
```

每个 `.pro` 可产出：`.fl`（折叠堆栈）、`.svg`（火焰图）、`.prof`（pprof），以及可选的 `.dot` / `.png` 调用图。

---

## 示例

### CPU 火焰图与调用图

![call](test/call.png)

### 内存分配图

![mem_ALLOC_SIZE](test/mem_ALLOC_SIZE.png)

---

## 相关项目

- [lua-family-bucket](https://github.com/esrrhs/lua-family-bucket)
- [hookso](https://github.com/esrrhs/hookso)（Linux 进程注入辅助工具）
