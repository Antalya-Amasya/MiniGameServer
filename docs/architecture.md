# 架构

这是一份用来讲清楚请求怎么走完的文档。数字和取舍以当前代码为准。

## 请求路径

```
Client
  |
  | TCP
  v
Reactor                         一个线程，epoll LT
  |  accept / recv / send
  v
Connection
  input buffer / output buffer
  |
  | 一行一条任务
  v
SessionLane                     同一连接严格按到达顺序
  |
  v
ThreadPool                      2 到 4 个 worker
  |
  v
Session
  |
  +--> RoomManager --> Room --> TicTacToeGame
  |
  +--> AccountService --> MySQL connection pool
  |
  v
ResponseQueue + eventfd
  |
  v
Reactor
  |
  v
OutputBuffer
  |
  v
Client
```

Reactor 线程不做 bcrypt，也不执行 SQL。Worker 不调用 `send`，也不改 epoll 兴趣列表。

## 为什么是这条链路

Phase 1 用阻塞 socket，每个连接一个读线程。连接一多，线程数就跟着涨。

Phase 2 把网络收成一个 epoll 循环。业务如果还在这个线程里做，一次慢登录会卡住所有连接。

Phase 3 把业务放进固定线程池。同一个连接的命令不能并行，所以每条连接有自己的 `SessionLane`。回包不能只记 fd，因为 fd 会复用，所以每条连接有递增的 connection id。Worker 把结果丢进 `ResponseQueue`，用 `eventfd` 叫醒 Reactor。

Phase 4 在 worker 上加了 MySQL。房间锁在 SQL 之前放开。

Phase 5 没有改这条链路。测量说明慢的是 bcrypt 和数据库往返，不是 Reactor，也不是那把房间锁。

## 缓冲和上限

- 一行最长 1024 字节。更长就回 `INVALID_COMMAND` 并关闭连接。
- 每个连接的命令队列最多 32 条，再多回 `SERVER_BUSY` 并关闭。
- 线程池队列最多 1024 个任务，满了就拒绝提交。
- 每个连接的 output buffer 最多 256 KiB。对端不读时，写到这个上限就关连接。
- 空闲默认 60 秒，由 `timerfd` 检查。`--idle-ms` 可以改短，最短 200 毫秒。

## 所有权

`Connection` 持有 `shared_ptr<Session>`。丢进线程池的任务再持有一份，所以任务执行时 Session 还在。房间里只留 `weak_ptr`，避免连接和房间互相持有。回包如果发现 connection id 已经不在表里，就丢掉，避免把字节写进后来复用的 fd。

## 关闭顺序

1. `stop()` 或 `SIGTERM` 标记 Reactor 停止，并写 `eventfd`。
2. 不再 `accept`。
3. 线程池做完已经入队的任务，然后 join。
4. Reactor 把还能写的回包写出去。
5. 关闭连接、监听 socket、`timerfd`、`eventfd`、epoll fd。
6. `main` 销毁 MySQL 连接池。借出中的连接先归还，再 `mysql_close`。

## 数据库连接

池的大小固定为 4。借出时先 `mysql_ping`。失败就关掉这条连接，最多新建 3 次。不会无限重试。自动重连开关是关着的，避免库自己在 ping 里面悄悄换连接。

## 一局的身份

`game_records` 用 `(room_id, started_at)` 做唯一键。进程内房间号只增不减，一个房间只打一局，这足够保证同一次运行里不会写重。服务器重启后房间号从 1001 重新计，如果同一秒又产生了同一个房间号，第二次插入会被当成已经写过。这个窗口很窄，Phase 5 没有为此再加 UUID。
