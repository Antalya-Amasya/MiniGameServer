# MiniGameServer

A Linux C++17 multiplayer Tic-Tac-Toe server built around an epoll Reactor, worker threads, MySQL persistence, and lifecycle validation.

## Overview

一个用 C++17 写的 Linux 多人井字棋服务器，用来演示网络、并发和 MySQL，而不是一个生产游戏平台。它在本机压力测试下验证过，规模是几十到一百个并发客户端。

演进是：

1. 阻塞 TCP，每连接一个线程
2. 非阻塞 socket，epoll，Reactor
3. 线程池，每连接顺序队列，心跳和关闭顺序
4. MySQL，注册登录，对局记录
5. 性能测量，内存检查，背压和数据库重连

业务功能到 Phase 4 为止。Phase 5 之后是功能冻结，只修缺陷和文档。

## Features

- epoll Reactor，水平触发
- 非阻塞读写，处理半包、粘包和部分发送
- 固定大小线程池
- 同一连接上的命令按到达顺序执行
- `timerfd` 空闲超时，可选 `PING` / `PONG`
- MySQL 连接池，预处理语句
- bcrypt 注册和登录
- 对局记录、`HISTORY`、`STATS`

Windows 仍能编译原来的 `select` 后端，不包含 MySQL。

## Architecture

```
Client
  |
  v
Reactor (epoll LT, 单线程)
  |
  v
Connection
  input buffer / output buffer
  |
  v
SessionLane
  |
  v
ThreadPool (2-4 workers)
  |
  +--> RoomManager -> Room -> TicTacToeGame
  |
  +--> MySQL pool (4 connections, worker only)
  |
  v
ResponseQueue + eventfd
  |
  v
Reactor -> OutputBuffer -> Client
```

细节在 `docs/architecture.md`。

## Networking

监听 socket 和非阻塞连接都在 Reactor 里。`LineBuffer` 按换行切包。写不完的字节留在 `OutputBuffer`，并用 `EPOLLOUT` 继续写。一行最长 1024 字节。输出缓冲最多 256 KiB。

## Concurrency

Reactor 只做 I/O。Worker 执行登录、房间、落子和 SQL。同一连接的任务进 `SessionLane`，不同连接可以并行。回包用 connection id，不用 fd。锁的顺序是会话命令锁，然后房间锁；SQL 在房间锁外面。

## Persistence

有 `.env.local` 时启用数据库，账号是 `minigame_app`。口令只存 bcrypt。终局写一行 `game_records`，`(room_id, started_at)` 唯一。借出连接时做 `mysql_ping`，失败最多重建 3 次。没有这份配置时，`LOGIN <name>` 仍只在内存里。说明见 `docs/persistence.md`。

## Safety

- ASan 和 UBSan：独立构建，Linux 回归通过
- Valgrind：普通 Debug 构建，见 Phase 5 报告
- 线程池、每连接命令队列、输出缓冲都有上限
- 数据库密码不进 git，日志不打印口令

## Testing

Linux CTest 目标：`unit`、`integration`、`stream`、`reactor`、`phase3`、`phase4`、`phase5_limits`。Debug 和 Release 都是 7/7 通过。

Windows MinGW 上跑过 `unit`、`integration`、`stream`、`phase5_limits`，4/4 通过。Windows 不编译 Linux 的 epoll 和 MySQL 代码。

## Performance

Release 构建，本机 WSL，正式段 10 秒，前面有 2 秒预热。延迟单位是毫秒。完整表在 `docs/performance-results.md`。

连接建立，100 个客户端，约 4533 次/秒，p99 约 38 毫秒。

带 bcrypt 的登录，10 个客户端约 20 次/秒，平均约 365 毫秒。50 个客户端时平均超过 1 秒，因为 4 个 worker 排上了队。100 个客户端时大量请求在 3 秒客户端超时里失败，进程没有退出。

这些数字只说明这台机器上的这次运行。

## Build

Linux：

```bash
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure

cmake -S . -B build-linux-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux-release --parallel
```

AddressSanitizer：

```bash
cmake -S . -B build-linux-asan -DMINIGAME_ENABLE_SANITIZERS=ON
cmake --build build-linux-asan --parallel
```

Windows 使用已安装、且路径不含空格的 MinGW，生成的是不带 MySQL 的后端。

## Run

```bash
# 数据库名来自环境。压测必须先指定测试库，避免写入 minigame。
MINIGAME_DB_NAME=minigame_test ./build-linux-release/bin/minigame_server --port 9000
```

没有 `.env.local` 时服务器仍会启动，只是不持久化。空闲时间可以用 `--idle-ms` 调短，最小 200。

客户端是 `minigame_client`。协议表在 `docs/protocol.md`。

## Protocol

未配置数据库：`LOGIN <name>`。

配置数据库之后：

- `REGISTER <username> <password>`
- `LOGIN <username> <password>`
- `HISTORY [limit]`
- `STATS`

用户名 1 到 16 位。密码 6 到 128 位可见 ASCII，不能含空格。其余命令是 `CREATE_ROOM`、`JOIN_ROOM`、`MOVE`、`PING`、`QUIT`。

## Project Structure

- `include/minigame`、`src`：服务器
- `client`：命令行客户端
- `tests`：单元、集成、Reactor、线程和数据库测试
- `sql/schema.sql`：表结构
- `scripts/stress_server.py`：压测客户端，不是服务器依赖
- `docs`：协议、线程、持久化、架构、性能和面试笔记

`.env.local` 和 `*.local.sql` 被 git 忽略。

## Known Limitations

- 房间共用一把互斥锁。测量里它不是热点，所以没有拆成每房间一把锁。
- bcrypt cost 12 让登录在几十个并发时就开始排队。
- 连接池固定 4 条。重连最多 3 次，没有主从或自动故障转移。
- 重启后房间号从 1001 再计，和 `started_at` 的秒精度一起，存在一个很窄的记录键冲突窗口。
- 没有断线重连，没有匹配、聊天或排行榜。
- 这台 WSL 没有 `perf`。TSan 此前会在启动时因为 unexpected memory mapping 退出。
- 不能把它描述成生产级或百万连接服务器。
