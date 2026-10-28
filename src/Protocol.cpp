#include "minigame/Protocol.hpp"

#include <vector>

namespace minigame {
namespace proto {
namespace {

std::vector<std::string> tokenize(std::string_view line) {
  std::vector<std::string> tokens;
  std::string current;
  for (char ch : line) {
    if (ch == ' ' || ch == '\t') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) {
    tokens.push_back(std::move(current));
  }
  return tokens;
}

// 只接受非负整数。坐标 0 是合法的，所以不能要求正整数。
bool parseIndex(const std::string& text, int& value) {
  if (text.empty() || text.size() > 9) {
    return false;
  }
  int parsed = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    parsed = parsed * 10 + (ch - '0');
  }
  value = parsed;
  return true;
}

Command invalid(std::string detail) {
  Command command;
  command.type = CommandType::Invalid;
  command.detail = std::move(detail);
  return command;
}

}  // namespace

bool isValidPlayerName(std::string_view name) {
  if (name.empty() || name.size() > kMaxNameLength) {
    return false;
  }
  for (char ch : name) {
    const bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    const bool digit = (ch >= '0' && ch <= '9');
    if (!letter && !digit && ch != '_') {
      return false;
    }
  }
  return true;
}

bool isValidPassword(std::string_view password) {
  if (password.size() < kMinPasswordLength || password.size() > kMaxPasswordLength) {
    return false;
  }
  for (unsigned char ch : password) {
    if (ch < 0x21 || ch > 0x7e) {
      return false;
    }
  }
  return true;
}

Command parseCommand(std::string_view line) {
  const std::vector<std::string> tokens = tokenize(line);
  if (tokens.empty()) {
    return invalid("empty command");
  }

  const std::string& verb = tokens[0];
  if (verb == kCmdLogin) {
    if (tokens.size() != 2 && tokens.size() != 3) {
      return invalid("usage: LOGIN <name> [password]");
    }
    Command command;
    command.type = CommandType::Login;
    command.name = tokens[1];
    if (tokens.size() == 3) {
      command.password = tokens[2];
    }
    return command;
  }
  if (verb == kCmdRegister) {
    if (tokens.size() != 3) {
      return invalid("usage: REGISTER <username> <password>");
    }
    Command command;
    command.type = CommandType::Register;
    command.name = tokens[1];
    command.password = tokens[2];
    return command;
  }
  if (verb == kCmdCreateRoom) {
    if (tokens.size() != 1) {
      return invalid("usage: CREATE_ROOM");
    }
    Command command;
    command.type = CommandType::CreateRoom;
    return command;
  }
  if (verb == kCmdJoinRoom) {
    if (tokens.size() != 2) {
      return invalid("usage: JOIN_ROOM <roomId>");
    }
    int roomId = 0;
    if (!parseIndex(tokens[1], roomId)) {
      return invalid("room id must be an integer");
    }
    Command command;
    command.type = CommandType::JoinRoom;
    command.roomId = roomId;
    return command;
  }
  if (verb == kCmdMove) {
    if (tokens.size() != 3) {
      return invalid("usage: MOVE <row> <col>");
    }
    int row = 0;
    int col = 0;
    if (!parseIndex(tokens[1], row) || !parseIndex(tokens[2], col)) {
      return invalid("row and col must be integers");
    }
    Command command;
    command.type = CommandType::Move;
    command.row = row;
    command.col = col;
    return command;
  }
  if (verb == kCmdHistory) {
    if (tokens.size() > 2) {
      return invalid("usage: HISTORY [limit]");
    }
    int limit = 20;
    if (tokens.size() == 2 && !parseIndex(tokens[1], limit)) {
      return invalid("history limit must be an integer");
    }
    if (limit < 1 || limit > 50) {
      return invalid("history limit must be from 1 to 50");
    }
    Command command;
    command.type = CommandType::History;
    command.limit = limit;
    return command;
  }
  if (verb == kCmdStats) {
    if (tokens.size() != 1) {
      return invalid("usage: STATS");
    }
    Command command;
    command.type = CommandType::Stats;
    return command;
  }
  if (verb == kCmdQuit) {
    if (tokens.size() != 1) {
      return invalid("usage: QUIT");
    }
    Command command;
    command.type = CommandType::Quit;
    return command;
  }
  if (verb == kCmdPing) {
    if (tokens.size() != 1) {
      return invalid("usage: PING");
    }
    Command command;
    command.type = CommandType::Ping;
    return command;
  }
  return invalid("unknown command");
}

}  // namespace proto
}  // namespace minigame
