#pragma once

#include "minigame/AccountService.hpp"
#include "minigame/PlayerLink.hpp"
#include "minigame/Room.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace minigame {

struct LoginStatus {
  bool ok = false;
  std::string code;
  std::string detail;
};

struct CreateRoomStatus {
  bool ok = false;
  int roomId = 0;
  std::string code;
  std::string detail;
};

struct JoinRoomStatus {
  bool ok = false;
  int roomId = 0;
  char mark = '?';
  std::string code;
  std::string detail;
};

struct MoveStatus {
  bool ok = false;
  MoveError error = MoveError::Ok;
  std::string code;
  std::string detail;
};

// 大厅：登录名、房间表、session 到房间的映射。
//
// 所有权：
// - rooms_ 用 unique_ptr，房间只属于 RoomManager。
// - 玩家用 shared_ptr 传入，房间内部只留 weak_ptr，避免房间和连接互相持有。
//
// 线程安全：
// - 所有 public 方法共用 mu_。
// - 持有 mu_ 时可以调用 PlayerLink::send / onRoomChanged。
// - send 和 onRoomChanged 禁止再进入 RoomManager。
// 锁顺序只有一种：RoomManager::mu_ -> Session 的发送锁。
class RoomManager {
 public:
  LoginStatus login(std::uint64_t sessionId, const std::string& name);
  void logout(std::uint64_t sessionId);

  CreateRoomStatus createRoom(const std::shared_ptr<PlayerLink>& player);
  JoinRoomStatus joinRoom(int roomId, const std::shared_ptr<PlayerLink>& player);
  MoveStatus move(std::uint64_t sessionId, int row, int col);

  std::size_t roomCount() const;
  std::size_t loggedInCount() const;

  // 只能在服务器启动前设置。未设置时登录和房间仍完全在内存中。
  void setAccounts(AccountService* accounts);
  AccountService* accounts() const;

 private:
  mutable std::mutex mu_;
  int nextRoomId_ = 1001;
  std::unordered_map<std::uint64_t, std::string> sessionNames_;
  std::unordered_map<std::string, std::uint64_t> nameToId_;
  std::unordered_map<int, std::unique_ptr<Room>> rooms_;
  std::unordered_map<std::uint64_t, int> sessionToRoom_;
  AccountService* accounts_ = nullptr;
};

}  // namespace minigame
