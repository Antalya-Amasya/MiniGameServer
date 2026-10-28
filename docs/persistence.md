# 持久化

Linux 上如果存在 `.env.local`，服务器会连接 MySQL，并使用 `minigame_app` 账号。这个文件和真正的口令不进 git。没有这份配置时，`LOGIN <name>` 仍只记在内存里，原来的测试不连数据库。

表结构在 `sql/schema.sql`：

- `users`：用户名唯一，保存 bcrypt 口令哈希、创建时间和最后登录时间。
- `game_records`：一局一条。`(room_id, started_at)` 唯一，重复插入不会多出一行。

对局在终局那一步写入。房间锁会先放开，再由 worker 执行预处理语句。Reactor 线程不访问 MySQL。

`STATS` 直接对 `game_records` 做聚合，没有单独的统计表。`HISTORY` 只返回当前账号参加过的对局。

数据库不可用时，相关命令返回 `DATABASE_UNAVAILABLE`，进程继续运行。

借出一条连接时会 `mysql_ping`。失败就关闭它，最多新建 3 次。不会无限重试。这个检查多一次网络往返，用来换连接断开后的恢复。

`game_records` 的唯一键是 `(room_id, started_at)`。同一次进程里房间号递增，而且一个房间只打一局，所以不会写重。没有再加 UUID。重启后如果同一秒复用了同一个房间号，第二次插入会被当成已经保存。
