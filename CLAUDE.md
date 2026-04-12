# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository context

This is a **Windows-port fork** of [cloudwu/skynet](https://github.com/cloudwu/skynet). The `vs2013` branch is cut from upstream tag `v1.8.0` and adds Visual Studio 2013 build support plus Windows-specific shims so the skynet actor-model Lua framework runs on Windows with minimal changes to upstream source.

Deltas relative to upstream:

1. **Windows build via VS2013 (SP4 required)** —— 只官方支持 VS2013，其它 VS 版本不保证
2. **`epoll` 用 `select` 模拟** —— shim 在 [build/vs2013/posix/cpoll/](build/vs2013/posix/cpoll/)
3. **`pipe()` 用 loopback socket pair 模拟** —— Win32 无 pipe API 可被 socket poller 复用
4. **`read(fd 0)` 被 hack** —— Windows 下可从 stdin 读取控制台输入
5. **sproto 扩展**：加了 `real`（double）和 `variant`（real/int/string/bool）两种 field 类型

## Build

### Windows（本 fork 主平台）

- 用 Visual Studio 2013（**必须 SP4**）打开 [build/vs2013/skynet.sln](build/vs2013/skynet.sln)，F7 Build All
- 产物在 `build/vs2013/bin/win32/<Configuration>/`
- 仓库根执行 [copybin.bat](copybin.bat) `<Debug|Release>`，把 exe / dll / cservice / luaclib 按 Linux 布局复制到根
- 运行：`skynet.exe examples\config`（另一个终端：`lua.exe examples\client.lua`）
- VS F5 调试已配好（见 [build/vs2013/skynet.vcxproj.user](build/vs2013/skynet.vcxproj.user)，per-developer 且 gitignore）

### Linux / macOS / FreeBSD

和上游一致：`make linux`（或 `macosx`、`freebsd`）。首次构建要 `git submodule update --init` 取 jemalloc（macOS 自动跳过 jemalloc）。

[test/](test/) 目录是手动测试的 Lua demo，不是 CI 测试套件。

## Runtime model（上游继承，简要）

单进程 + 固定 worker 线程池（默认 `thread = 8`） + 3 个专用线程（timer / socket / monitor），详见 [skynet-src/skynet_start.c](skynet-src/skynet_start.c)。每个 service 是一个 actor（`skynet_context`），有独立消息队列，worker 抢队列、drain 批消息、调 callback。服务之间只靠 `skynet_send` / `skynet_sendname` 通信。

`PTYPE_*` 常量在 [skynet-src/skynet.h:12-25](skynet-src/skynet.h#L12-L25)，**必须与 [lualib/skynet.lua:32-40](lualib/skynet.lua#L32-L40) 保持一致** —— 新增 PTYPE 要同步改两处。

启动流程：[skynet-src/skynet_main.c](skynet-src/skynet_main.c) → 解析 config（本身是 Lua）→ `skynet_start` 初始化 → launch `bootstrap` 服务（默认 `snlua bootstrap`）→ [service/bootstrap.lua](service/bootstrap.lua) 拉起 `.launcher` / harbor / `service_mgr` → 启动用户 `start` 服务。

## Platform shims（Windows）

- [build/vs2013/posix/](build/vs2013/posix/) —— POSIX 兼容层（`unistd.h`、`pthread.h`、`dlfcn.h`、`arpa/`、`netinet/`、`sys/`）
- [build/vs2013/posix/cpoll/](build/vs2013/posix/cpoll/) —— `select` 实现的 epoll 模拟
- [skynet-src/socket_cpoll.h](skynet-src/socket_cpoll.h) —— `sp_*` 抽象在 Windows 下绑 cpoll
- [skynet-src/socket_server_select.c](skynet-src/socket_server_select.c) —— 调度层，正常 `#include socket_server.c`，`USE_IOCP && _WIN32` 时 include `socket_server_iocp.c`（IOCP 实验性）
- [skynet-src/skynet_compat.h](skynet-src/skynet_compat.h) —— MSVC 兼容宏

## Conventions to preserve when editing

### C source（[skynet-src/](skynet-src/)）

- **不要丢 `_MSC_VER` / `_WIN32` guard**。多处函数有 MSVC / POSIX 并行分支（定长栈数组 vs VLA、`srand`、`sigemptyset` 缺失等），丢了 Windows 就挂。
- **不要直接调 `epoll_*` / `kqueue` / 原生 Win32 socket API**，全部走 [skynet-src/socket_poll.h](skynet-src/socket_poll.h) 里的 `sp_*` 抽象，三个后端才能保持同步。

### Lua（[lualib/](lualib/) / [service/](service/)）

- 用户代码 `require "skynet.core"` 和 `lualib/skynet.lua` 的公开接口，不要碰内部 C API。
- Lua 侧入口 [lualib/skynet.lua](lualib/skynet.lua)，子模块在 [lualib/skynet/](lualib/skynet/)（socket、cluster、sharedata、snax、debug 等）。

### 添加新的 Lua C 扩展

1. `.c` 放进 [lualib-src/](lualib-src/)（或子目录）
2. 在相应 vcxproj 加 `<ClCompile>` —— 合并进 `skynet.so` 就改 [build/vs2013/lualib/skynet.vcxproj](build/vs2013/lualib/skynet.vcxproj)，独立 `.so` 就新建一个 vcxproj
3. 合并进 `skynet.so` 时还要在 [build/vs2013/lualib/skynet.def](build/vs2013/lualib/skynet.def) 加 `luaopen_skynet_<name>`
4. [Makefile](Makefile) 对应条目跟上（Linux 侧）
5. 重新 build、`copybin.bat`、从 Lua `require` 测一下

### 编码

[README.md](README.md) 已清理为纯 UTF-8，**不要再引入 CP936/GBK 段**（原始 fork 的 README 有 mojibake，已整理）。

### `.gitignore` 不要删这几条

这几条保护构建产物和 per-developer 配置不入库：

- `/skynet.exe`、`/lua.exe`、`/lua.dll`、`/posix.dll` —— `copybin.bat` 拷到根的产物
- `*.vcxproj.user` —— per-developer VS 调试设置
- `build/vs2013/output/`、`build/vs2013/bin/` —— VS 中间 / 输出目录
