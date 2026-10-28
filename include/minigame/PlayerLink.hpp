#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace minigame {

// 房间只依赖这个窄接口发消息，不直接依赖 socket。
// 这样房间逻辑可以用假玩家做测试，Session 则是真正的网络实现。
class PlayerLink {
 public:
  virtual ~PlayerLink() = default;

  virtual std::uint64_t id() const = 0;
  virtual const std::string& name() const = 0;
  // 数据库账号。没有持久化时是 0，房间据此跳过写库。
  virtual std::uint64_t accountId() const { return 0; }
  virtual void send(std::string_view text) = 0;

  // roomId 为 0 表示当前不在任何房间。
  // 实现里只允许改自己的字段，不能再回调 RoomManager，否则会和房间锁死锁。
  virtual void onRoomChanged(int roomId) = 0;
};

}  // namespace minigame
