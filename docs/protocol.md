# 文本协议

连接是 TCP 长连接。一条消息是一行 UTF-8 文本，以 `\n` 结束。`\r` 会被去掉。

命令区分大小写。空行会被忽略。单行超过 1024 个字符时，服务器返回错误并断开这条连接，不影响其他连接。

棋盘坐标从 0 开始，行和列的范围都是 `0..2`。

## 客户端命令

| 命令 | 含义 |
| --- | --- |
| `LOGIN <name>` | 未配置数据库时的内存登录。名字是 1 到 16 位字母、数字或下划线 |
| `REGISTER <username> <password>` | 配置数据库后注册。密码 6 到 128 位可见 ASCII，不能含空格 |
| `LOGIN <username> <password>` | 配置数据库后的登录 |
| `HISTORY [limit]` | 自己的最近对局，limit 默认 20，范围 1 到 50 |
| `STATS` | 自己的场次、胜、负、平 |
| `CREATE_ROOM` | 创建房间，创建者执 X |
| `JOIN_ROOM <roomId>` | 加入等待中的房间，加入者执 O |
| `MOVE <row> <col>` | 落子 |
| `QUIT` | 退出并断开 |

## 服务器消息

| 消息 | 含义 |
| --- | --- |
| `OK LOGIN <name>` | 登录成功 |
| `OK REGISTER <name>` | 注册成功，不会自动登录 |
| `HISTORY <count>` 然后若干行 `<id> <result> <time>`，最后 `END_HISTORY` | 历史对局 |
| `STATS games=<n> wins=<n> losses=<n> draws=<n>` | 汇总 |
| `ROOM_CREATED <roomId>` | 房间已创建 |
| `WAITING_OPPONENT` | 等待第二名玩家 |
| `ROOM_JOINED <roomId> O` | 已加入房间，执 O |
| `OPPONENT_JOINED <name>` | 对手进入房间 |
| `GAME_START <mark>` | 对局开始，`<mark>` 是你的棋子 |
| `BOARD` 加随后三行 | 当前棋盘，空位是 `.` |
| `YOUR_TURN` | 轮到你 |
| `OPPONENT_TURN` | 轮到对手 |
| `GAME_OVER X_WIN` | X 获胜 |
| `GAME_OVER O_WIN` | O 获胜 |
| `GAME_OVER DRAW` | 平局 |
| `GAME_OVER OPPONENT_LEFT` | 对手离开，本局中止 |
| `OPPONENT_LEFT` | 对手断开 |
| `OK QUIT` | 退出确认 |
| `ERROR <CODE> <detail>` | 请求被拒绝 |

## 错误码

`INVALID_COMMAND`、`NOT_LOGGED_IN`、`BAD_NAME`、`ALREADY_LOGGED_IN`、`NAME_TAKEN`、`ALREADY_ONLINE`、`USER_EXISTS`、`INVALID_CREDENTIALS`、`DATABASE_UNAVAILABLE`、`INTERNAL_SERVER_ERROR`、`ALREADY_IN_ROOM`、`ROOM_NOT_FOUND`、`ROOM_FULL`、`NOT_IN_ROOM`、`GAME_NOT_STARTED`、`NOT_YOUR_TURN`、`OUT_OF_RANGE`、`CELL_OCCUPIED`、`GAME_OVER`。

没有配置数据库时，`LOGIN <name>` 仍是原来的内存登录。配置数据库后，同名第二次在线登录返回 `ALREADY_ONLINE`。

这些字符串定义在 `include/minigame/Protocol.hpp`。

## 对局结束后的状态

正常分出胜负或平局后，两名玩家仍留在房间里。再次 `MOVE` 会得到 `ERROR GAME_OVER`。想再开一局需要 `QUIT` 后重新连接。

如果有一名玩家断开，房间会被删除。还在线上的另一名玩家会收到 `OPPONENT_LEFT`，并回到大厅，可以再次 `CREATE_ROOM`。
