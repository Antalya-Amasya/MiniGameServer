#include "minigame/RoomManager.hpp"

#include "minigame/Logger.hpp"
#include "minigame/Protocol.hpp"

namespace minigame {
namespace {

void setMoveError(MoveStatus& status, MoveError error) {
  status.ok = false;
  status.error = error;
  switch (error) {
    case MoveError::OutOfRange:
      status.code = proto::kOutOfRange;
      status.detail = "row and col must be 0..2";
      break;
    case MoveError::CellOccupied:
      status.code = proto::kCellOccupied;
      status.detail = "cell already used";
      break;
    case MoveError::WrongTurn:
      status.code = proto::kNotYourTurn;
      status.detail = "wait for the other player";
      break;
    case MoveError::GameFinished:
      status.code = proto::kGameOver;
      status.detail = "the game is already finished";
      break;
    case MoveError::Ok:
      break;
  }
}

}  // namespace

LoginStatus RoomManager::login(std::uint64_t sessionId, const std::string& name) {
  std::lock_guard<std::mutex> lock(mu_);
  LoginStatus status;
  if (sessionNames_.find(sessionId) != sessionNames_.end()) {
    status.code = proto::kAlreadyLoggedIn;
    status.detail = "name already set";
    return status;
  }
  if (!proto::isValidPlayerName(name)) {
    status.code = proto::kBadName;
    status.detail = "use 1-16 letters, digits, or underscore";
    return status;
  }
  if (nameToId_.find(name) != nameToId_.end()) {
    status.code = proto::kNameTaken;
    status.detail = "choose another name";
    return status;
  }

  sessionNames_.emplace(sessionId, name);
  nameToId_.emplace(name, sessionId);
  status.ok = true;
  return status;
}

void RoomManager::logout(std::uint64_t sessionId) {
  std::lock_guard<std::mutex> lock(mu_);
  const auto nameIt = sessionNames_.find(sessionId);
  if (nameIt != sessionNames_.end()) {
    nameToId_.erase(nameIt->second);
    sessionNames_.erase(nameIt);
  }

  const auto roomIt = sessionToRoom_.find(sessionId);
  if (roomIt == sessionToRoom_.end()) {
    return;
  }

  const int roomId = roomIt->second;
  const auto found = rooms_.find(roomId);
  if (found == rooms_.end()) {
    sessionToRoom_.erase(sessionId);
    return;
  }

  const DisconnectResult removed = found->second->handleDisconnect(sessionId);
  sessionToRoom_.erase(sessionId);
  if (removed.partnerId != 0) {
    sessionToRoom_.erase(removed.partnerId);
  }
  if (removed.removed) {
    rooms_.erase(roomId);
    log::info("room removed " + std::to_string(roomId));
  }
}

CreateRoomStatus RoomManager::createRoom(const std::shared_ptr<PlayerLink>& player) {
  std::lock_guard<std::mutex> lock(mu_);
  CreateRoomStatus status;
  if (!player || sessionNames_.find(player->id()) == sessionNames_.end()) {
    status.code = proto::kNotLoggedIn;
    status.detail = "send LOGIN <name> first";
    return status;
  }
  if (sessionToRoom_.find(player->id()) != sessionToRoom_.end()) {
    status.code = proto::kAlreadyInRoom;
    status.detail = "leave the current room first";
    return status;
  }

  const int roomId = nextRoomId_;
  ++nextRoomId_;
  std::unique_ptr<Room> room = std::make_unique<Room>(roomId);
  Room& roomRef = *room;
  rooms_.emplace(roomId, std::move(room));
  sessionToRoom_.emplace(player->id(), roomId);
  // 先放入表，再发 ROOM_CREATED。这样客户端收到成功消息时，房间已经可以被查找。
  roomRef.seatFirstPlayer(player);

  status.ok = true;
  status.roomId = roomId;
  log::info("room created " + std::to_string(roomId) + " by " + player->name());
  return status;
}

JoinRoomStatus RoomManager::joinRoom(int roomId, const std::shared_ptr<PlayerLink>& player) {
  std::lock_guard<std::mutex> lock(mu_);
  JoinRoomStatus status;
  if (!player || sessionNames_.find(player->id()) == sessionNames_.end()) {
    status.code = proto::kNotLoggedIn;
    status.detail = "send LOGIN <name> first";
    return status;
  }
  if (sessionToRoom_.find(player->id()) != sessionToRoom_.end()) {
    status.code = proto::kAlreadyInRoom;
    status.detail = "leave the current room first";
    return status;
  }

  const auto found = rooms_.find(roomId);
  if (found == rooms_.end()) {
    status.code = proto::kRoomNotFound;
    status.detail = "no such room";
    return status;
  }
  if (!found->second->canJoin()) {
    status.code = proto::kRoomFull;
    status.detail = "room is not waiting for a player";
    return status;
  }

  sessionToRoom_.emplace(player->id(), roomId);
  found->second->seatSecondPlayer(player);
  status.ok = true;
  status.roomId = roomId;
  status.mark = 'O';
  log::info("room joined " + std::to_string(roomId) + " by " + player->name());
  return status;
}

MoveStatus RoomManager::move(std::uint64_t sessionId, int row, int col) {
  MoveStatus status;
  FinishedRecord record;
  bool persist = false;
  {
    // 数据库调用放在这把锁外面。锁内只改房间状态。
    std::lock_guard<std::mutex> lock(mu_);
    const auto roomIt = sessionToRoom_.find(sessionId);
    if (roomIt == sessionToRoom_.end()) {
      status.code = proto::kNotInRoom;
      status.detail = "join a room first";
      return status;
    }

    const auto found = rooms_.find(roomIt->second);
    if (found == rooms_.end()) {
      sessionToRoom_.erase(sessionId);
      status.code = proto::kNotInRoom;
      status.detail = "join a room first";
      return status;
    }

    const ApplyMoveResult applied = found->second->applyMove(sessionId, row, col);
    if (applied.notMember) {
      status.code = proto::kNotInRoom;
      status.detail = "join a room first";
      return status;
    }
    if (applied.notStarted) {
      status.code = proto::kGameNotStarted;
      status.detail = "waiting for the opponent";
      return status;
    }
    if (!applied.ok) {
      setMoveError(status, applied.error);
      return status;
    }

    status.ok = true;
    status.error = MoveError::Ok;
    if (applied.gameEnded) {
      log::info("game over room " + std::to_string(roomIt->second));
      if (accounts_ != nullptr) {
        persist = found->second->exportUnsaved(record);
      }
    }
  }

  if (persist && accounts_ != nullptr) {
    if (!accounts_->saveGame(record)) {
      log::error("game record was not saved room " + std::to_string(record.roomId));
    } else {
      std::lock_guard<std::mutex> lock(mu_);
      const auto found = rooms_.find(record.roomId);
      if (found != rooms_.end()) {
        found->second->markRecordSaved();
      }
    }
  }
  return status;
}

void RoomManager::setAccounts(AccountService* accounts) {
  accounts_ = accounts;
}

AccountService* RoomManager::accounts() const {
  return accounts_;
}

std::size_t RoomManager::roomCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return rooms_.size();
}

std::size_t RoomManager::loggedInCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return sessionNames_.size();
}

}  // namespace minigame
