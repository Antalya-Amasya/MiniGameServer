# Reactor

这份说明只描述当前仓库里的实现。

## 为什么 Phase 1 用每连接一个线程

Phase 1 要先把登录、房间和井字棋跑通。监听线程用 `select` 等新连接，每个 `Session::run()` 在自己的线程里阻塞 `recv`。这样读循环和业务代码都是顺序的，方便在 Windows 上验证。Windows 没有 epoll，所以这条路径还留着。

## 为什么 Phase 2 改成 Reactor

阻塞读要为每个连接占一个线程。连接变多时，线程切换和阻塞 `recv` 都在等数据。Linux 上可以让一个线程用 `epoll` 同时等很多 fd：谁有数据再读谁。

## blocking 和 non-blocking

阻塞 socket 上，`recv` 在没有数据时会停住调用它的线程。非阻塞 socket 上，没有数据时 `recv` 立刻返回 `EAGAIN` 或 `EWOULDBLOCK`，线程可以回去继续 `epoll_wait`。

Linux 上监听 socket 和 accept 出来的 socket 都会 `fcntl` 设置 `O_NONBLOCK`。`TcpSocket::recvSome` 把 `EAGAIN` 收成返回值 `-2`。`sendSome` 也一样。`EINTR` 会重试，不会当成断线。

## epoll 和 Reactor

`Epoll` 持有 `epoll_create1` 得到的 fd，析构时关闭。`Reactor::loop` 是唯一的事件循环：

1. `epoll_wait` 无限等待。
2. 监听 fd 可读就 `accept`，直到 `EAGAIN`。
3. 连接 fd 可读就读进 `Connection` 的 input buffer。
4. 输出缓冲区还有数据就关注 `EPOLLOUT`，可写时继续写。
5. `EPOLLERR`、`EPOLLHUP`、`EPOLLRDHUP`，以及 `recv == 0`，走一次关闭。

这是水平触发（LT）：只要条件还在，下一次 `epoll_wait` 还会报告。Phase 2 不用边缘触发（ET）。ET 要求每次事件都把缓冲区读空或写空，漏一次就会再也等不到。LT 更容易把半包和 partial write 写对。

## input buffer

TCP 不保留 `send` 的边界。`LineBuffer` 把字节攒起来，按 `\n` 切帧。

- `LOGIN alice\n` 一次收完，得到一条命令。
- `LOGIN alice\nCREATE_ROOM\n` 一次收完，得到两条。
- 先收到 `LOG`，再收到 `IN alice\n`，拼成 `LOGIN alice`。
- `MOVE 0` 没有换行，继续留在缓冲区里。

一行或未完成的一段超过 `proto::kMaxLineLength`（1024）时，连接收到错误并关闭，不会让缓冲区无限增长。

## output buffer

`send` 也可能只写出一部分。`Session::send` 在 Linux 上只是把字节追加到 `OutputBuffer`。`flush` 调用 `sendSome`。返回 `-2` 就把剩下的留下，并给这个 fd 加上 `EPOLLOUT`。写完后取消 `EPOLLOUT`，避免空转。写失败则关闭连接。

## fd 归谁

| fd | 谁创建 | 谁持有 | 谁关闭 |
| --- | --- | --- | --- |
| 监听 socket | `TcpSocket::listenOn` | 移交给 `Reactor` | 事件循环结束时 `close` |
| 客户端 socket | `accept` | `Connection` 里的 `TcpSocket` | 先 `EPOLL_CTL_DEL`，再由 `TcpSocket` 析构 `close`。`close()` 可以重复调用 |
| epoll fd | `epoll_create1` | `Epoll` | `Epoll` 析构 |
| eventfd | `eventfd` | `Reactor` | 循环结束时 `close` |

同一条连接的 `beginClose` 只会把 `closed_` 设为 true 一次。`Session::finish` 也只会 `logout` 一次。

`Server::stop()` 往 eventfd 写 8 个字节。`epoll_wait` 因此返回，循环看到唤醒标记后退出。不靠超时轮询。

## 为什么还没有线程池

Reactor 线程里直接调用 `Session::handleLine`，再进 `RoomManager`。这样生命周期简单：没有“IO 线程把任务丢给另一个线程，连接却先被销毁”的问题。`RoomManager` 的互斥锁先留着，后面如果把落子放到工作线程，还需要它。一个很慢的业务调用现在会挡住整个事件循环。这是 Phase 3 再拆的事。
