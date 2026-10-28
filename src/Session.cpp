#include "minigame/Session.hpp"

#include "minigame/Logger.hpp"
#include "minigame/Protocol.hpp"
#include "minigame/RoomManager.hpp"

namespace minigame {

std::atomic<std::uint64_t> Session::nextId_{1};

Session::Session(TcpSocket socket, RoomManager& rooms)
    : socket_(std::move(socket)),
      rooms_(rooms),
      id_(nextId_.fetch_add(1)) {}

Session::Session(RoomManager& rooms) : rooms_(rooms), id_(nextId_.fetch_add(1)) {}

Session::~Session() {
  closing_.store(true);
  std::lock_guard<std::mutex> lock(sendMu_);
  socket_.close();
}

void Session::run() {
  online_.store(true);
  log::info("session start id=" + std::to_string(id_));
  try {
    readLoop();
  } catch (const std::exception& ex) {
    log::error(std::string("session exception id=") + std::to_string(id_) + " " + ex.what());
  } catch (...) {
    log::error("session unknown exception id=" + std::to_string(id_));
  }

  // 无论是 QUIT、对端断开还是读失败，都从这里清理房间和登录名。
  // logout 可以重复调用，但这条路径只会进来一次。
  finish();
}

void Session::setOutbound(std::function<void(std::string_view)> outbound) {
  outbound_ = std::move(outbound);
}

bool Session::handleLine(const std::string& line) {
  std::lock_guard<std::mutex> lock(commandMu_);
  if (finished_) {
    return false;
  }
  return dispatch(line);
}

void Session::finish() {
  std::lock_guard<std::mutex> lock(commandMu_);
  if (finished_) {
    return;
  }
  finished_ = true;
  rooms_.logout(id_);
  online_.store(false);
  log::info("session end id=" + std::to_string(id_));
}

void Session::requestStop() {
  // 只 shutdown，不 close。读线程还阻塞在 recv 时，fd 不能被释放后立刻复用。
  // close 留在析构里，而析构发生在 run() 返回之后。
  closing_.store(true);
  socket_.shutdownBoth();
}

std::uint64_t Session::id() const {
  return id_;
}

const std::string& Session::name() const {
  return name_;
}

std::uint64_t Session::accountId() const {
  return accountId_;
}

void Session::send(std::string_view text) {
  // Linux Reactor 上发送只是把字节放进 Connection 的 output buffer。
  // 回调里不能再进入 RoomManager。
  if (outbound_) {
    outbound_(text);
    return;
  }
  std::lock_guard<std::mutex> lock(sendMu_);
  if (!socket_.valid()) {
    return;
  }
  if (!socket_.sendAll(text) && !closing_.load()) {
    log::warn("send failed session=" + std::to_string(id_));
  }
}

void Session::onRoomChanged(int newRoomId) {
  roomId_.store(newRoomId);
}

int Session::roomId() const {
  return roomId_.load();
}

bool Session::online() const {
  return online_.load();
}

void Session::readLoop() {
  std::string buffer;
  char temp[512];
  while (!closing_.load()) {
    const int received = socket_.recvSome(temp, static_cast<int>(sizeof(temp)));
    if (received == 0) {
      log::info("peer closed session=" + std::to_string(id_));
      return;
    }
    if (received == -2) {
      continue;
    }
    if (received < 0) {
      if (!closing_.load()) {
        log::warn("recv failed session=" + std::to_string(id_));
      }
      return;
    }

    buffer.append(temp, temp + received);
    for (;;) {
      const std::size_t pos = buffer.find('\n');
      if (pos == std::string::npos) {
        break;
      }
      if (pos > proto::kMaxLineLength) {
        send(proto::error(proto::kInvalidCommand, "line too long"));
        return;
      }
      std::string line = buffer.substr(0, pos);
      buffer.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.find_first_not_of(" \t") == std::string::npos) {
        continue;
      }
      if (handleLine(line)) {
        return;
      }
    }

    if (buffer.size() > proto::kMaxLineLength) {
      send(proto::error(proto::kInvalidCommand, "line too long"));
      return;
    }
  }
}

bool Session::dispatch(const std::string& line) {
  const proto::Command command = proto::parseCommand(line);
  if (command.type == proto::CommandType::Invalid) {
    send(proto::error(proto::kInvalidCommand, command.detail));
    return false;
  }
  if (command.type == proto::CommandType::Quit) {
    send(proto::okQuit());
    return true;
  }
  if (command.type == proto::CommandType::Ping) {
    send(proto::pong());
    return false;
  }
  if (command.type == proto::CommandType::Register) {
    AccountService* accounts = rooms_.accounts();
    if (accounts == nullptr) {
      send(proto::error(proto::kInvalidCommand, "database is not configured"));
      return false;
    }
    if (!proto::isValidPlayerName(command.name)) {
      send(proto::error(proto::kBadName, "username must be 1-16 letters, digits, or underscore"));
      return false;
    }
    if (!proto::isValidPassword(command.password)) {
      send(proto::error(proto::kInvalidCredentials, "password must be 6-128 printable characters"));
      return false;
    }
    const AuthResult created = accounts->registerAccount(command.name, command.password);
    if (!created.ok) {
      send(proto::error(created.code, created.detail));
      return false;
    }
    send(proto::withNewline("OK REGISTER " + command.name));
    return false;
  }
  if (command.type == proto::CommandType::Login) {
    AccountService* accounts = rooms_.accounts();
    if (accounts == nullptr) {
      if (!command.password.empty()) {
        send(proto::error(proto::kInvalidCommand, "usage: LOGIN <name>"));
        return false;
      }
      const LoginStatus status = rooms_.login(id_, command.name);
      if (!status.ok) {
        send(proto::error(status.code, status.detail));
        return false;
      }
      name_ = command.name;
      send(proto::okLogin(name_));
      log::info("login session=" + std::to_string(id_) + " name=" + name_);
      return false;
    }
    if (!proto::isValidPlayerName(command.name) || !proto::isValidPassword(command.password)) {
      send(proto::error(proto::kInvalidCredentials, "invalid username or password"));
      return false;
    }
    const AuthResult auth = accounts->authenticate(command.name, command.password);
    if (!auth.ok) {
      send(proto::error(auth.code, auth.detail));
      return false;
    }
    accountId_ = auth.accountId;
    const LoginStatus status = rooms_.login(id_, command.name);
    if (!status.ok) {
      accountId_ = 0;
      const char* code = status.code == proto::kNameTaken ? proto::kAlreadyOnline : status.code.c_str();
      const char* detail = status.code == proto::kNameTaken ? "this account is already online" : status.detail.c_str();
      send(proto::error(code, detail));
      return false;
    }
    name_ = command.name;
    send(proto::okLogin(name_));
    log::info("login session=" + std::to_string(id_) + " name=" + name_);
    return false;
  }
  if (name_.empty()) {
    send(proto::error(proto::kNotLoggedIn, "send LOGIN <name> first"));
    return false;
  }
  if (command.type == proto::CommandType::History || command.type == proto::CommandType::Stats) {
    AccountService* accounts = rooms_.accounts();
    if (accounts == nullptr || accountId_ == 0) {
      send(proto::error(proto::kNotLoggedIn, "send LOGIN <username> <password> first"));
      return false;
    }
    if (command.type == proto::CommandType::History) {
      const HistoryResult history = accounts->history(accountId_, command.limit);
      if (!history.ok) {
        send(proto::error(history.code, history.detail));
        return false;
      }
      send(proto::withNewline("HISTORY " + std::to_string(history.rows.size())));
      for (const HistoryEntry& entry : history.rows) {
        send(proto::withNewline(std::to_string(entry.id) + " " + entry.result + " " + entry.finishedAt));
      }
      send("END_HISTORY\n");
      return false;
    }
    const StatsResult stats = accounts->stats(accountId_);
    if (!stats.ok) {
      send(proto::error(stats.code, stats.detail));
      return false;
    }
    send(proto::withNewline("STATS games=" + std::to_string(stats.games) + " wins=" + std::to_string(stats.wins) +
                            " losses=" + std::to_string(stats.losses) + " draws=" + std::to_string(stats.draws)));
    return false;
  }

  if (command.type == proto::CommandType::CreateRoom) {
    const CreateRoomStatus status = rooms_.createRoom(shared_from_this());
    if (!status.ok) {
      send(proto::error(status.code, status.detail));
    }
    return false;
  }
  if (command.type == proto::CommandType::JoinRoom) {
    const JoinRoomStatus status = rooms_.joinRoom(command.roomId, shared_from_this());
    if (!status.ok) {
      send(proto::error(status.code, status.detail));
    }
    return false;
  }
  if (command.type == proto::CommandType::Move) {
    const MoveStatus status = rooms_.move(id_, command.row, command.col);
    if (!status.ok) {
      send(proto::error(status.code, status.detail));
    }
    return false;
  }
  return false;
}

}  // namespace minigame
