#pragma once

#include "minigame/PlayerLink.hpp"
#include "minigame/TicTacToeGame.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace minigame {

struct FinishedRecord {
  int roomId = 0;
  std::uint64_t playerX = 0;
  std::uint64_t playerO = 0;
  char winner = 'D';
  std::string result;
  std::int64_t startedAt = 0;
};

}  // namespace minigame

namespace minigame {

enum class RoomStatus { Waiting, Playing, Finished };

// applyMove 的结果只描述规则，不拼协议文本。
// 协议错误码由 RoomManager 统一映射。
struct ApplyMoveResult {
  bool ok = false;
  bool notStarted = false;
  bool notMember = false;
  bool gameEnded = false;
  MoveError error = MoveError::Ok;
};

struct DisconnectResult {
  bool removed = false;
  std::uint64_t partnerId = 0;
};

// Room 自己不加锁。
// Phase 1 由 RoomManager 的一把互斥锁串行化所有房间操作，避免锁顺序出错。
// 房间不拥有玩家：座位里保存 weak_ptr，玩家生命周期归 Server 里的 shared_ptr。
class Room {
 public:
  explicit Room(int id);

  Room(const Room&) = delete;
  Room& operator=(const Room&) = delete;

  int id() const;
  RoomStatus status() const;
  bool canJoin() const;
  bool contains(std::uint64_t sessionId) const;

  void seatFirstPlayer(const std::shared_ptr<PlayerLink>& player);
  void seatSecondPlayer(const std::shared_ptr<PlayerLink>& player);
  ApplyMoveResult applyMove(std::uint64_t sessionId, int row, int col);

  // 有人离开时通知还在房间里的对手，并清空座位。
  // removed 为 false 表示这个 session 本来就不在房间里，调用方不要删房间。
  DisconnectResult handleDisconnect(std::uint64_t sessionId);
  bool exportUnsaved(FinishedRecord& out) const;
  void markRecordSaved();

 private:
  struct Seat {
    std::weak_ptr<PlayerLink> link;
    std::uint64_t id = 0;
    std::uint64_t accountId = 0;
    std::string name;
    bool occupied = false;
  };

  std::string statePacket(Mark viewer) const;
  void clearSeat(Seat& seat);

  int id_ = 0;
  RoomStatus status_ = RoomStatus::Waiting;
  Seat x_;
  Seat o_;
  TicTacToeGame game_;
  std::int64_t startedAt_ = 0;
  bool recordSaved_ = false;
};

}  // namespace minigame
