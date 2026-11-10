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
- **CPU 归因**：POSIX 使用 `ITIMER_PROF` / `SIGPROF` tick 和安全上下文中的 Lua 栈快照，可归因 CPU 密集 C 调用。Windows（或强制回退）采集 VM 指令计数，**不能测量 C CPU 时间**。精度限制见下文。
- **多 State**：每个 `lua_State` 拥有独立会话，可在同一进程中对多个 State 分别采样。**多 State ≠ 多 OS 线程安全**——采样操作应在同一 OS 线程进行，进程 CPU 限制见下文。
- **CPU / 内存独立开关**：`cpuRunning` / `memRunning` 分离，同一 State 上可同时开启。
- **可配置采样率**：`set_config` / `get_config`、环境变量，以及 `start` / `start_mem` 的可选速率参数。
- **运行时状态查询**：`status()` 可查看 CPU/内存是否在采、样本数、输出路径、当前后端与配置快照；`close()` 用于释放当前 `lua_State` 上的会话。
- **可调开销**：采样频率可调；call/return 栈快照的额外开销取决于工作负载。
- **C 回调仍可归因到 Lua**：安全边界快照跳过 C 帧（参考 [vLua](https://github.com/esrrhs/vLua)），保留最近的 Lua 帧（已有的源文件/函数名/函数定义位置）。这不是完整的原生 C 栈展开，只恢复 Lua 侧信息。
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

**线程安全：** 所有 pLua/Lua 操作限于同一 OS 线程，会话和归因快照不支持跨线程。POSIX 定时器测量的是**进程 CPU**：即使 Lua 访问加锁，其他活跃原生线程仍会污染归因。并行运行的 SIGPROF 会话必须使用相同间隔，冲突的 start 会返回错误。

---

### 4. Hook 独占与协程边界

**`lua_sethook` 独占。** pLua 通过 `lua_sethook` 安装调试钩子做 CPU 采样（内存采样武装时也会短暂使用）。在某个 `lua_State` 上采样期间，它假定 **独占** 该 State 的 hook：

- **不要**在同一 State 上同时运行其它也会调用 `lua_sethook` 的调试器 / 分析器 / 覆盖率工具——彼此会互相覆盖，导致采样缺失或对方工具失效。
- 在接入其它基于 hook 的工具之前，请先 `stop` / `stop_mem` / `close` 停止 pLua。

**协程。** Lua 5.2–5.4 创建新线程时可能复制 debug hook，但这**不代表**创建了 pLua 会话。需要分析的协程应在自身内部显式 `start` / `stop`。已注册的协程会被保活，直到 stop/close 或全局 `lua_close`。

- 在开始采样**之后**创建的未注册协程，在支持的 Lua 版本上会继承边界 hook；其 CPU tick 会被排除，重复 yield/resume 也一样。
- **预先存在且没有 hook 的协程，以及从 C 进入的未插桩 Lua State，公共 API 无法检测。** 其耗时可能归给外层 `coroutine.resume` / C 桥的 Lua 调用者。应显式注册，或解读父线程结果时排除这类工作负载。
- 尾调用会从 Lua debug 栈中抹掉调用者；pLua 不会虚构已消失的帧。

会话回收：stop/close 只注销当前会话；按线程保存的 registry anchor 会在全局 `lua_close` 时清理仍活跃的会话。停止一个协程不会使另一个协程的 anchor 失效。

**C 耗时精度（POSIX）。** call、return、count hook 在正常 VM 上下文里生成 Lua/main 帧快照，跳过 C 帧。信号处理函数只递增无锁计数器，**不调用 Lua API、不分配、不加锁、不遍历会话容器、不做 native 栈回溯**。下一个边界先把所有已投递 tick 记到**旧快照**，再替换快照。因此长 C 调用可以保留多次 tick，并归给其 Lua 调用者，而不是返回后的无关 Lua 工作；Lua→C→Lua 在回调进入/返回时切换归因。直接在 C 中调用 `lrealstop(L)` 也会收集 pending tick 并落盘；只能在 VM 线程的安全上下文调用，不能从信号处理函数调用。

这并非 vLua 基于 VM 内部结构的精确中断栈采样：两次 hook 之间快照可能过时，错误展开/continuation 可能跳过 return hook，hook 本身也有额外开销（尤其是调用密集、深栈代码）。POSIX 信号仍可能在**内核中合并**，被阻塞/延迟的信号无法重建。没有可恢复 Lua 帧的纯 C 工作被排除；观察到的 VM 退出/进入之间没有 owner 时，宿主 CPU 不计入，未观察到的切换仍有上述限制。pLua 独占 `SIGPROF` / `ITIMER_PROF`，不能和其他使用者共用。CPU 时间是已投递 tick × 配置间隔的估计，不是精确耗时。count 后端只采 Lua VM 指令，不能测量 C 时间。

---

### 5. 运行时状态

```lua
local st = p.status()
-- st.cpu_running / st.mem_running  (boolean)
-- st.cpu_samples / st.mem_samples  (number，当前已采样本数)
-- st.cpu_output / st.mem_output    (路径或 nil)
-- st.cpu_backend                   ("sigprof" 或 "count")
-- st.config                        (与 get_config() 同结构)

p.close()  -- 如有需要会先 stop cpu/mem，并释放本 State 上的会话
```

`start` / `start_mem` 失败时返回 `-1`，并在第二个返回值给出错误说明（例如已在运行、缺少输出路径）。

---

### 6. 可配置采样率

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

## `.pro` 文件格式（CPU v2，内存 v1）

现有小端格式的整数均为 32 位。CPU v2 头部是六个 `uint32`：magic `0x41554C50`（`PLUA`）、version `2`、kind `1`（CPU）、backend `1`（SIGPROF）/ `2`（count）、毫秒间隔、名义 VM hook 指令步长。这样无需按文件名猜 CPU 单位。内存仍输出 v1（magic + version `1`）；v0 没有头部。

负载不变：每条记录为 count/权重、深度和 64 个根帧在前的 frame ID；尾部字符串表为若干 `(名称字节、字节长度、ID)`，最后为条目数。空 CPU profile 也有合法尾表。SIGPROF 权重是已投递 tick 数，count 权重是 hook 样本数，内存权重是采样分配字节/估计存活字节。`tools/plua` 支持 v0/v1/v2，拒绝未知版本和畸形/截断记录；旧转换器需更新后才能读取 v2。

---

## 可视化与工具

### `tools/plua`

Go 转换工具（安装了 Go 时会编译到 `bin/plua`）：

```shell
# 火焰图折叠堆栈
./bin/plua -i call.pro -flame call.fl

# 旧版 gperftools 符号化 profile（-pprof 保持不变）
./bin/plua -i call.pro -pprof call.prof

# 现代 gzip profile.proto（仅 CPU v2，无第三方 Go 依赖）
./bin/plua -i call.pro -proto call.pb.gz
go tool pprof -top call.pb.gz
go tool pprof -sample_index=samples -top call.pb.gz

# 同时生成
./bin/plua -i call.pro -flame call.fl -proto call.pb.gz -pprof call.prof
```

`-collapsed` 是 `-flame` 的别名。`-proto` 输出叶帧在前的栈，function/location ID 合法且字符串表索引 0 为空。SIGPROF 导出 `samples/count` 和估计 `cpu/nanoseconds`、CPU period，权重为 count × 间隔。count 后端只导出 `samples/count`，period 为名义 `instructions/count`，绝不虚构 CPU 纳秒；同时开启内存采样可能缩短部分 count hook 间隔。不虚构时间戳或持续时间。

`-proto` 明确拒绝内存以及旧版/v1 文件，因为它们缺少可靠的类型、后端和周期元数据。这些文件继续用 `-flame` / 旧 `-pprof`；内存值应解释为字节，而不是 CPU 样本/时间。现有 `show.sh` 仍使用旧格式输出。

### 精度与转换器测试

```shell
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
(cd tools && go test ./... -v)
# 可选统计实验，不作为 CI 时间阈值：
./bin/precision_test --experiment
```

确定性测试注入已投递 SIGPROF tick，精确检查权重/叶帧归因：长 C 边界批次、C→Lua 回调、pending stop、尾调用消帧、协程重复恢复、全局关闭/重启和独立 State。Windows 仅运行 count 后端检查。转换器测试覆盖元数据、单位、栈顺序、ID、截断/畸形输入、旧格式/火焰图输出，并真实调用 `go tool pprof -top` 验证 CPU/count fixture。可选实验消耗 600ms C CPU，报告相对名义 60 tick 的偏差，不使用易抖动的统计断言。

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
