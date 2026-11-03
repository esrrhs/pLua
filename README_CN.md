# pLua

[English](README.md) | [中文文档](README_CN.md)

[<img src="https://img.shields.io/github/license/esrrhs/pLua">](https://github.com/esrrhs/pLua)
[<img src="https://img.shields.io/github/languages/top/esrrhs/pLua">](https://github.com/esrrhs/pLua)
[<img src="https://img.shields.io/github/actions/workflow/status/esrrhs/pLua/ccpp.yml?branch=master">](https://github.com/esrrhs/pLua/actions)

Lua 性能及内存分析工具

## 简介
类似于 `gperftools`，**pLua** 可以分析 Lua 程序的热点函数及内存分配情况。

## 特性
- **简单**：只需几行代码即可开启采样。在 Linux 上还可通过 [hookso](https://github.com/esrrhs/hookso) 动态注入。
- **准确**：POSIX 上使用 `ITIMER_PROF` / `SIGPROF` 定时采样；Windows（及其他可选场景）回退到 `LUA_MASKCOUNT`。
- **多 State**：每个 `lua_State` 独立会话，可并行采样。**多 State ≠ 多 OS 线程安全**。
- **CPU / 内存独立开关**：可同时开启，不再共用一个 `gRunning`。
- **轻量 / 直观**：采样开销小，兼容 `pprof`，可生成火焰图。

## 平台

| 平台 | 产物 | CPU 后端 | hookso | 说明 |
|------|------|----------|--------|------|
| Linux | `bin/libplua.so` | SIGPROF（默认） | 支持 | 主平台 |
| macOS | `bin/libplua.so` | SIGPROF（默认） | **不支持** | 嵌入可用 |
| Windows | `bin/libplua.dll` | MASKCOUNT | **不支持** | 嵌入可用 |

### Lua 版本
支持 **Lua 5.2 / 5.3 / 5.4**。当前构建**不支持 Lua 5.1 / LuaJIT**（旧文档中的兼容声明不准确）。

## 编译

```shell
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

## 使用

```lua
package.cpath = "./bin/?.so;./bin/?.dylib;./bin/?.dll;" .. package.cpath
local p = require "libplua"  -- 也可在库名为 plua.so 时 require "plua"

p.start(0, "call.pro")       -- 可选第 3 参：间隔毫秒
-- ...
p.stop()

p.start_mem(0, "mem.pro")    -- 可选第 3 参：采样字节率
collectgarbage("collect")
p.stop_mem()

p.set_config({ cpu_interval_ms = 10, mem_rate = 524288, cpu_backend = "sigprof" })
```

环境变量：`PLUA_CPU_INTERVAL_MS`、`PLUA_CPU_HOOK_COUNT`、`PLUA_MEM_RATE`、`PLUA_CPU_BACKEND`。

`.pro` 文件带 `PLUA` magic + version；`tools/plua` 兼容旧格式。

hookso 注入仅 Linux；macOS / Windows 请使用嵌入方式。

## 相关项目
- [lua-family-bucket](https://github.com/esrrhs/lua-family-bucket)
