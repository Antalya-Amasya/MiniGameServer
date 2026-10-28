#include "minigame/Room.hpp"

#include "minigame/Protocol.hpp"

#include <ctime>

namespace minigame {

Room::Room(int id) : id_(id) {}

int Room::id() const {
  return id_;
}

RoomStatus Room::status() const {
  return status_;
}

bool Room::canJoin() const {
  return status_ == RoomStatus::Waiting && x_.occupied && !o_.occupied;
}

bool Room::contains(std::uint64_t sessionId) const {
  const bool inX = x_.occupied && x_.id == sessionId;
  const bool inO = o_.occupied && o_.id == sessionId;
  return inX || inO;
}

void Room::seatFirstPlayer(const std::shared_ptr<PlayerLink>& player) {
  x_.link = player;
  x_.id = player->id();
  x_.accountId = player->accountId();
  x_.name = player->name();
  x_.occupied = true;
  status_ = RoomStatus::Waiting;
  player->onRoomChanged(id_);
  player->send(proto::roomCreated(id_) + proto::waitingOpponent());
}

void Room::seatSecondPlayer(const std::shared_ptr<PlayerLink>& player) {
  o_.link = player;
  o_.id = player->id();
  o_.accountId = player->accountId();
  o_.name = player->name();
  o_.occupied = true;
  status_ = RoomStatus::Playing;
  startedAt_ = std::time(nullptr);
  player->onRoomChanged(id_);

  if (std::shared_ptr<PlayerLink> first = x_.link.lock()) {
    std::string toFirst = proto::opponentJoined(o_.name);
    toFirst += proto::gameStart('X');
    toFirst += statePacket(Mark::X);
    first->send(toFirst);
  }

  std::string toSecond = proto::roomJoined(id_, 'O');
  toSecond += proto::gameStart('O');
  toSecond += statePacket(Mark::O);
  player->send(toSecond);
}

ApplyMoveResult Room::applyMove(std::uint64_t sessionId, int row, int col) {
  ApplyMoveResult result;
  const bool isX = x_.occupied && x_.id == sessionId;
  const bool isO = o_.occupied && o_.id == sessionId;
  if (!isX && !isO) {
    result.notMember = true;
    return result;
  }
  if (status_ == RoomStatus::Waiting) {
    result.notStarted = true;
    return result;
  }
  if (status_ == RoomStatus::Finished || game_.finished()) {
    result.error = MoveError::GameFinished;
    return result;
  }

  const Mark mark = isX ? Mark::X : Mark::O;
  const MoveOutcome outcome = game_.tryMove(mark, row, col);
  if (outcome.error != MoveError::Ok) {
    result.error = outcome.error;
    return result;
  }

  if (game_.finished()) {
    status_ = RoomStatus::Finished;
    result.gameEnded = true;
  }

  // 一次 send 带上棋盘和回合/结果，避免两条消息中间被这个玩家自己的错误响应插进去。
  if (std::shared_ptr<PlayerLink> first = x_.link.lock()) {
    first->send(statePacket(Mark::X));
  }
  if (std::shared_ptr<PlayerLink> second = o_.link.lock()) {
    second->send(statePacket(Mark::O));
  }

  result.ok = true;
  result.error = MoveError::Ok;
  return result;
}

DisconnectResult Room::handleDisconnect(std::uint64_t sessionId) {
  DisconnectResult result;
  Seat* self = nullptr;
  Seat* other = nullptr;
  if (x_.occupied && x_.id == sessionId) {
    self = &x_;
    other = &o_;
  } else if (o_.occupied && o_.id == sessionId) {
    self = &o_;
    other = &x_;
  } else {
    return result;
  }

  result.removed = true;
  if (other->occupied) {
    result.partnerId = other->id;
    if (std::shared_ptr<PlayerLink> partner = other->link.lock()) {
      std::string message = proto::opponentLeft();
      message += proto::gameOver(proto::kResultOpponentLeft);
      partner->send(message);
      partner->onRoomChanged(0);
    }
    clearSeat(*other);
  }
  if (std::shared_ptr<PlayerLink> leaver = self->link.lock()) {
    leaver->onRoomChanged(0);
  }
  clearSeat(*self);
  status_ = RoomStatus::Finished;
  return result;
}

std::string Room::statePacket(Mark viewer) const {
  std::string out = proto::boardMessage(game_.renderBoard());
  if (game_.finished()) {
    out += proto::gameOver(resultToken(game_.state()));
    return out;
  }
  if (game_.currentMark() == viewer) {
    out += proto::yourTurn();
  } else {
    out += proto::opponentTurn();
  }
  return out;
}

bool Room::exportUnsaved(FinishedRecord& out) const {
  if (status_ != RoomStatus::Finished || recordSaved_ || startedAt_ == 0) {
    return false;
  }
  if (x_.accountId == 0 || o_.accountId == 0) {
    return false;
  }
  out.roomId = id_;
  out.playerX = x_.accountId;
  out.playerO = o_.accountId;
  out.startedAt = startedAt_;
  out.result = resultToken(game_.state());
  if (game_.state() == GameState::XWin) {
    out.winner = 'X';
  } else if (game_.state() == GameState::OWin) {
    out.winner = 'O';
  } else {
    out.winner = 'D';
  }
  return true;
}

void Room::markRecordSaved() {
  recordSaved_ = true;
}

void Room::clearSeat(Seat& seat) {
  seat.link.reset();
  seat.id = 0;
  seat.name.clear();
  seat.occupied = false;
}

}  // namespace minigame
