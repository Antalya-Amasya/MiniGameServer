# 线程、超时和关闭

这里只写当前代码里的做法。

## Reactor 线程做什么

`Reactor::loop` 跑在一个线程上。它负责：

- `epoll_wait`
- `accept`
- 把读到的字节放进 `LineBuffer`
- 把完整的一行投递给这个连接自己的 `SessionLane`
- 把 worker 送回来的字节写进 `OutputBuffer`，再 `send`
- 读 `timerfd`，关掉空闲连接
- 读 `eventfd`，处理停止和“有回包了”

它不再调用 `Session::handleLine`。

## Worker 做什么

`ThreadPool` 的数量是 `std::thread::hardware_concurrency()`，再限制在 2 到 4。井字棋的命令很短，更多线程对正确性没有帮助。

Worker 执行 `Session::handleLine`：登录、房间、落子，以及数据库读写。`send` 不会写 socket，只调用一个回调，把字符串放进 `ResponseQueue`，再写 `eventfd`。

MySQL 只出现在 worker 线程。Reactor 不拿连接池。房间锁在发起 SQL 之前会放开，避免一个慢查询卡住所有房间。

## 为什么不让 worker 写 socket

`OutputBuffer`、`epoll_ctl` 和 fd 都假设只有 Reactor 线程会碰。worker 如果直接 `send`，会和 `EPOLLOUT` 的 flush 抢同一块缓冲。所以 worker 只生产结果，Reactor 再写。

## 命令顺序

每个连接有一个 `SessionLane`。队列里的任务一次只跑一个，跑完再取下一个。同一个客户端的 `MOVE 0 0` 和后面的命令不会被两个 worker 同时执行。不同连接可以并行。

## connection id

每次 `accept` 分配一个递增的 `uint64_t`。回包里带的是这个 id，不是 fd。fd 关掉以后可以被下一个连接复用。Reactor 回写前会查表：id 对不上，或者连接已经关了，就把结果丢掉。

## Session 生命周期

`Connection` 持有 `shared_ptr<Session>`。投进线程池的任务再持有一份 `shared_ptr`，所以任务执行期间 Session 不会被析构。

`handleLine` 和 `finish` 共用 `commandMu_`。断开时把 `finish` 排进同一条 `SessionLane`，排在已经收到的命令后面。`finish` 只会 `logout` 一次。已经结束的 Session 再收到命令会直接返回。

worker 不保存 `Connection*`。它只保存 connection id 和 `Reactor*`。`Reactor` 在 `ThreadPool::stop()` 返回之前一直活着。

## 房间锁

`RoomManager` 仍用一把互斥锁。锁里面会调用 `Session::send`，那只是往响应队列里 push，不会再去等 Reactor，也不会去拿 `commandMu_`。顺序是：`commandMu_`，然后 `RoomManager` 的锁，然后响应队列的锁。

## 空闲超时

`timerfd` 注册在 epoll 上。间隔是空闲时间的一半，最短 100 毫秒，最长 1 秒。默认空闲时间是 60 秒，`Server::setIdleTimeout` 可以改短，测试用这个。

每次 `recv` 到数据都会刷新活动时间，包括 `PING`。超时后 Reactor 关掉这条连接。`PING` 的回复是 `PONG`。

## 背压

- 线程池队列最多 1024 个任务，再提交会失败。
- 每个连接自己的命令队列最多 32 条。满了就回 `ERROR SERVER_BUSY` 并关闭连接。
- 每个连接的 output buffer 最多 256 KiB。再大就关闭连接。

## 关闭顺序

1. `Server::stop()` 把 Reactor 标成停止，并写 `eventfd`。
2. 循环不再 `accept`。
3. `ThreadPool::stop()` 不再收新任务，把队列里已有的任务做完，然后 join。无效 Session 的任务会自己退出。
4. Reactor 把剩下的回包写出去；连接已经不在了就丢掉。
5. 关闭连接、监听 socket、`timerfd`、`eventfd`、epoll fd。
6. `main` 里的 MySQL 连接池在 worker 已经 join 之后才销毁。

worker 里的任务如果抛出异常，会被记一条日志，线程继续干活，不会把进程杀掉。
