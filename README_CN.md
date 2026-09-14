# pLua

[English](README.md) | [中文文档](README_CN.md)

[<img src="https://img.shields.io/github/license/esrrhs/pLua">](https://github.com/esrrhs/pLua)
[<img src="https://img.shields.io/github/languages/top/esrrhs/pLua">](https://github.com/esrrhs/pLua)
[<img src="https://img.shields.io/github/actions/workflow/status/esrrhs/pLua/ccpp.yml?branch=master">](https://github.com/esrrhs/pLua/actions)

Lua 性能及内存分析工具

## 简介
类似于 `gperftools`，**pLua** 可以分析 Lua 程序的热点函数及内存分配情况。

## 特性
- **简单**：只需几行代码即可开启采样输出结果，或者通过 [hookso](https://github.com/esrrhs/hookso) 动态注入到目标进程，无需修改源码。
- **准确**：相比常规 Lua hook，定时采样的方式（基于 `ITIMER_PROF`）更能准确捕获 Lua 执行的 CPU 热点，并自动剔除 sleep 等空闲堆栈。
- **轻量**：基于采样机制，对宿主程序的运行性能影响极小。
- **直观**：输出调用关系图，兼容 `gperftools` 的 `pprof` 工具，可直接生成 [火焰图](https://github.com/brendangregg/FlameGraph)。

---

## 依赖安装

pLua 依赖系统中的 Lua 开发库及头文件（支持 Lua 5.1/5.2/5.3/5.4 及 LuaJIT）。请使用包管理器自行安装依赖：

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

## 编译构建

使用 CMake 进行标准构建：

```shell
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

编译生成的动态链接库位于 `bin/libplua.so`。如果环境已安装 Go，`tools/plua.go` 解析转换工具也会自动编译到 `bin/plua`。

---

## 单元测试

使用 CTest 运行自动化单元测试：

```shell
cd build
ctest --output-on-failure
```

或者直接运行 Lua 单元测试脚本：

```shell
cd test
lua test_unit.lua
```

---

## 使用说明

### 1. 获取 CPU 采样数据

#### 方式一：在 Lua 代码中嵌入
```lua
-- 引入 libplua.so
local p = require "libplua"

-- 开启采样
-- 参数 1：采样时间（秒），0 表示一直采样直到显式停止
-- 参数 2：采样结果输出文件路径
p.start(0, "call.pro")

do_some_thing()

-- 结束采样，输出结果文件
p.stop()
```

#### 方式二：使用 [hookso](https://github.com/esrrhs/hookso) 注入
```shell
# a) 获取进程中的 lua_State 指针（例如进程的 xxx.so 调用了 lua_settop(L) 函数，则取第 1 个参数）
./hookso arg $PID xxx.so lua_settop 1 
123456

# b) 加载 libplua.so
./hookso dlopen $PID ./libplua.so

# c) 执行 libplua.so 的 lrealstart 手动开启，等价于 lrealstart(L, 0, "./call.pro")
./hookso call $PID libplua.so lrealstart i=123456 i=0 s="./call.pro"

# d) 执行 libplua.so 的 lrealstop 手动关闭
./hookso call $PID libplua.so lrealstop i=123456
```

---

### 2. 获取内存采样数据

#### 方式一：在 Lua 代码中嵌入
```lua
-- 引入 libplua.so
local p = require "libplua"

-- 开启内存采样
-- 参数 1：采样时间（秒），0 表示一直采样
-- 参数 2：采样结果文件名前缀，会生成 2 个采样文件，分别代表内存分配大小(_ALLOC_SIZE.pro)与常驻内存占用大小(_USAGE.pro)
p.start_mem(0, "mem.pro")

do_some_thing()

-- 如需查看常驻内存占用，可在 stop 前执行垃圾回收
collectgarbage("collect")

-- 结束采样，输出结果文件
p.stop_mem()
```

#### 方式二：使用 [hookso](https://github.com/esrrhs/hookso) 注入
```shell
# a) 获取进程中的 lua_State 指针
./hookso arg $PID xxx.so lua_settop 1 
123456

# b) 加载 libplua.so
./hookso dlopen $PID ./libplua.so

# c) 执行 libplua.so 的 lrealstartmem 手动开启
./hookso call $PID libplua.so lrealstartmem i=123456 i=0 s="./mem.pro"

# d) 执行 libplua.so 的 lrealstopmem 手动关闭
./hookso call $PID libplua.so lrealstopmem i=123456
```

---

## 可视化分析工具

`tools/` 目录提供了将 `.pro` 采样数据转换为火焰图（SVG）及调用图（DOT/PNG）的脚本。

### 可选依赖
- **FlameGraph**：若本地及 PATH 中无 `flamegraph.pl`，`show.sh` 会尝试自动下载。CentOS 下可能需要 `yum install perl-open`。
- **Graphviz**（用于生成调用关系 PNG 图片）：`sudo apt install graphviz` / `sudo yum install graphviz`。
- **pprof**（用于生成 DOT 调用图）：`sudo apt install google-perftools` / `sudo yum install gperftools`。

### 批量生成可视化图表
```shell
cd tools
./show.sh ../test
```

输出产物包含：
- `<name>.fl`：折叠堆栈（Folded stack）文件
- `<name>.svg`：交互式 SVG 火焰图
- `<name>.prof`：pprof 符号化分析数据
- `<name>.dot`：Graphviz 调用图源码
- `<name>.png`：调用图 PNG 图片（在安装了 graphviz 和 pprof 时自动渲染）

---

## 示例

### CPU 调用热点图
由 `test/test_cpu.lua` 采样生成：

![call](test/call.png)

### 内存分配图
由 `test/test_mem.lua` 采样生成：

![mem_ALLOC_SIZE](test/mem_ALLOC_SIZE.png)

---

## 相关项目
- [lua-family-bucket](https://github.com/esrrhs/lua-family-bucket)
