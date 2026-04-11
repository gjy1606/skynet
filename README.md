## ![skynet logo](https://github.com/cloudwu/skynet/wiki/image/skynet_metro.jpg)

## 简介
- 基于 [cloudwu/skynet @tag v1.8.0](https://github.com/cloudwu/skynet/tree/v1.8.0) 版本为 `Windows` 提供支持
- 版本追踪：
  - [cloudwu/skynet @tag v1.8.0](https://github.com/cloudwu/skynet/tree/v1.8.0) => [gjy1606/skynet @vs2013](https://github.com/gjy1606/skynet/tree/vs2013) 进行最简 `Windows` 支持,无私有内容(有提PR的能力并且可以贡献给他人使用)
  - [gjy1606/skynet @vs2013](https://github.com/gjy1606/skynet/tree/vs2013) == [本地 guojiayi/skynet-hcplay @vs2013](http://192.168.10.252/guojiayi/skynet-hcplay) => [本地 guojiayi/skynet-hcplay @developer](http://192.168.10.252/guojiayi/skynet-hcplay/-/tree/developer) 进行项目私有内容支持

Skynet is a multi-user Lua framework supporting the actor model, often used in games.

[It is heavily used in the Chinese game industry](https://github.com/cloudwu/skynet/wiki/Uses), but is also now spreading to other industries, and to English-centric developers. To visit related sites, visit the Chinese pages using something like Google or Deepl translate.

The community is friendly and almost all contributors can speak English, so English speakers are welcome to ask questions in [Discussion](https://github.com/cloudwu/skynet/discussions), or submit issues in English.

## Build

For windows, open build/vs2013/skynet.sln and build all
You can use vs ide to debugging skynet

```
## Difference between offical skynet
1.sproto support real(double)/variant(real/int/string) field type
2.used event-select to simulate epoll
3.use socket api to simulate pipe()
4.hack read fd(0) for console input
```

For Linux, install autoconf first for jemalloc:

```
git clone https://github.com/cloudwu/skynet.git
cd skynet
make 'PLATFORM'  # PLATFORM can be linux, macosx, freebsd now
```

Or:

```
export PLAT=linux
make
```

For FreeBSD , use gmake instead of make.

## Test

Run these in different consoles:

For Linux / macOS / FreeBSD:

```
./skynet examples/config	# Launch first skynet node  (Gate server) and a skynet-master (see config for standalone option)
./3rd/lua/lua examples/client.lua 	# Launch a client, and try to input hello.
```

For Windows (after building the VS2013 solution):

```
copybin.bat Debug              # or: copybin.bat Release -- copy exe/dll/cservice/luaclib to repo root
skynet.exe examples\config     # Launch first skynet node
lua.exe    examples\client.lua # Launch a client, and try to input hello.
```

`copybin.bat` mirrors the Linux layout at the repo root from `build\vs2013\bin\win32\<Configuration>\`, so the command line is identical across platforms. Re-run it after each rebuild. You can also press F5 in Visual Studio to launch `skynet.exe examples\config` directly (see [build/vs2013/skynet.vcxproj.user](build/vs2013/skynet.vcxproj.user) for the debug command / working directory settings — this file is per-developer and not tracked).

## About Lua version

Skynet now uses a modified version of lua 5.4.7 ( https://github.com/ejoy/lua/tree/skynet54 ) for multiple lua states.

Official Lua versions can also be used as long as the Makefile is edited.

## How To Use

* Read Wiki for documents https://github.com/cloudwu/skynet/wiki (Written in both English and Chinese)
* The FAQ in wiki https://github.com/cloudwu/skynet/wiki/FAQ (In Chinese, but you can visit them using something like Google or Deepl translate.)
