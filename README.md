## ![skynet logo](https://github.com/cloudwu/skynet/wiki/image/skynet_metro.jpg)

Skynet is a multi-user Lua framework supporting the actor model, often used in games.

[It is heavily used in the Chinese game industry](https://github.com/cloudwu/skynet/wiki/Uses), but is also now spreading to other industries, and to English-centric developers. To visit related sites, visit the Chinese pages using something like Google or Deepl translate.

The community is friendly and almost all contributors can speak English, so English speakers are welcome to ask questions in [Discussion](https://github.com/cloudwu/skynet/discussions), or submit issues in English.

---

## 本分支定位（`vs2013`）

本分支是基于 [cloudwu/skynet @ v1.8.0](https://github.com/cloudwu/skynet/tree/v1.8.0) 的 **公共 Windows 适配分支**，无任何私有游戏代码，可以对 cloudwu/skynet 回馈 PR。

**版本追踪链：**

```
cloudwu/skynet @ v1.8.0
  ↓
github.com/gjy1606/skynet @ vs2013        （当前分支，公共 Windows 适配）
  ↓ 仅 push
gitlab.../skynet-hcplay @ vs2013          （内网 gitlab 上的镜像）
  ↓ 作为基础
gitlab.../skynet-hcplay @ developer       （私有游戏代码分支，不对外）
```

分支拓扑、Remote 配置、"禁止推送私有分支到 github"的防护策略，见 [CLAUDE.md](CLAUDE.md)。

---

## 相对上游 `cloudwu/skynet v1.8.0` 的改动

本分支对上游的改动**全部围绕 Windows 平台适配**，6 项：

1. **Visual Studio 2013 工程支持**：`build/vs2013/skynet.sln` 等一套 vcxproj / sln。仅支持 VS2013，**必须打上 SP4 补丁**，否则会编译失败。
2. **POSIX 兼容层**：`build/vs2013/posix/` 模拟 `unistd.h` / `pthread.h` / `dlfcn.h` / `arpa/` / `netinet/` / `sys/` 等头文件。
3. **`select` 模拟 `epoll`**：`build/vs2013/posix/cpoll/` 基于 `select(2)` 实现最小 epoll 抽象；`skynet-src/socket_cpoll.h` 把上游 `sp_*` 抽象在 Windows 下绑到 cpoll；`skynet-src/socket_server_select.c` 作为调度层 include `socket_server.c`。
4. **loopback socket pair 模拟 `pipe()`**：`build/vs2013/posix/unistd.c` 的 `pipe()` 用 `127.0.0.1` + 随机端口 `bind/listen/connect/accept` 构造一对 socket。
5. **stdin (`fd == 0`) 的 hack**：`build/vs2013/posix/unistd.c` 的 `read()` 在 `fd == 0` 时走 `_kbhit/_getch`；`cpoll.cpp` 里对 `fd == 0` 特判忽略，防止把 stdin 当 socket 处理。
6. **MSVC 兼容宏**：`skynet-src/skynet_compat.h` 提供关键字与内建函数的 MSVC 映射。

Linux / macOS / FreeBSD 路径上保持和上游完全一致，本分支不对非 Windows 平台做改动。

---

## Build

### Windows（本分支主要针对的平台）

1. 确认 Visual Studio 2013 + **SP4 补丁** 已安装。
2. 打开 [build/vs2013/skynet.sln](build/vs2013/skynet.sln)，Build All（`F7`）。
3. 产物位于 `build/vs2013/bin/win32/<Configuration>/`。
4. 在仓库根运行 [copybin.bat](copybin.bat) 把产物按 Linux 目录结构镜像到仓库根：
   ```bat
   copybin.bat Debug       REM 或 Release
   ```
5. 每次重新 build 后重跑 `copybin.bat`。也可以在 VS 里按 `F5` 直接启动（调试参数见 `build/vs2013/skynet.vcxproj.user`，per-developer 文件，不入库）。

### Linux / macOS / FreeBSD

和上游完全一样：

```sh
git clone https://github.com/cloudwu/skynet.git
cd skynet
make 'PLATFORM'    # PLATFORM: linux, macosx, freebsd
```

或：

```sh
export PLAT=linux
make
```

**FreeBSD 用 `gmake` 代替 `make`**。首次构建前先 `git submodule update --init` 取 jemalloc（macOS 会自动跳过 jemalloc）。

---

## Test

Run these in different consoles.

**Linux / macOS / FreeBSD：**

```sh
./skynet examples/config                 # Launch skynet node
./3rd/lua/lua examples/client.lua        # Launch a client, try typing hello
```

**Windows（build 完并 `copybin.bat` 后）：**

```bat
skynet.exe examples\config               REM 终端 1
lua.exe    examples\client.lua           REM 终端 2
```

`copybin.bat` 把 Windows 产物镜像到仓库根后，命令行就和 Linux 完全一致。

---

## About Lua version

Skynet now uses a modified version of **Lua 5.4.7**（<https://github.com/ejoy/lua/tree/skynet54>）for multiple lua states.

Official Lua versions can also be used as long as the Makefile is edited.

**注**：上游 `cloudwu/skynet` 已经升级到 Lua 5.5.0，本分支目前保持在 5.4.7 以匹配 v1.8.0 基线。

---

## How To Use

* Wiki 文档：<https://github.com/cloudwu/skynet/wiki>（中英双语）
* FAQ：<https://github.com/cloudwu/skynet/wiki/FAQ>（中文，可翻译查看）
