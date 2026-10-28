#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace minigame {
namespace proto {

// 文本协议的命令字和错误码集中在这里。
// 业务代码使用这些常量，避免魔法字符串散落在各个 cpp 中。

inline constexpr char kCmdLogin[] = "LOGIN";
inline constexpr char kCmdRegister[] = "REGISTER";
inline constexpr char kCmdCreateRoom[] = "CREATE_ROOM";
inline constexpr char kCmdJoinRoom[] = "JOIN_ROOM";
inline constexpr char kCmdMove[] = "MOVE";
inline constexpr char kCmdHistory[] = "HISTORY";
inline constexpr char kCmdStats[] = "STATS";
inline constexpr char kCmdQuit[] = "QUIT";
inline constexpr char kCmdPing[] = "PING";

inline constexpr char kInvalidCommand[] = "INVALID_COMMAND";
inline constexpr char kNotLoggedIn[] = "NOT_LOGGED_IN";
inline constexpr char kBadName[] = "BAD_NAME";
inline constexpr char kAlreadyLoggedIn[] = "ALREADY_LOGGED_IN";
inline constexpr char kNameTaken[] = "NAME_TAKEN";
inline constexpr char kAlreadyOnline[] = "ALREADY_ONLINE";
inline constexpr char kUserExists[] = "USER_EXISTS";
inline constexpr char kInvalidCredentials[] = "INVALID_CREDENTIALS";
inline constexpr char kDatabaseUnavailable[] = "DATABASE_UNAVAILABLE";
inline constexpr char kInternalServerError[] = "INTERNAL_SERVER_ERROR";
inline constexpr char kAlreadyInRoom[] = "ALREADY_IN_ROOM";
inline constexpr char kRoomNotFound[] = "ROOM_NOT_FOUND";
inline constexpr char kRoomFull[] = "ROOM_FULL";
inline constexpr char kNotInRoom[] = "NOT_IN_ROOM";
inline constexpr char kGameNotStarted[] = "GAME_NOT_STARTED";
inline constexpr char kNotYourTurn[] = "NOT_YOUR_TURN";
inline constexpr char kOutOfRange[] = "OUT_OF_RANGE";
inline constexpr char kCellOccupied[] = "CELL_OCCUPIED";
inline constexpr char kGameOver[] = "GAME_OVER";
inline constexpr char kServerBusy[] = "SERVER_BUSY";

inline constexpr char kResultOpponentLeft[] = "OPPONENT_LEFT";

inline constexpr std::size_t kMaxNameLength = 16;
inline constexpr std::size_t kMinPasswordLength = 6;
inline constexpr std::size_t kMaxPasswordLength = 128;
inline constexpr std::size_t kMaxLineLength = 1024;

enum class CommandType {
  Login,
  Register,
  CreateRoom,
  JoinRoom,
  Move,
  History,
  Stats,
  Quit,
  Ping,
  Invalid
};

struct Command {
  CommandType type = CommandType::Invalid;
  std::string name;
  std::string password;
  int roomId = 0;
  int row = 0;
  int col = 0;
  int limit = 20;
  std::string detail;
};

bool isValidPlayerName(std::string_view name);
bool isValidPassword(std::string_view password);
Command parseCommand(std::string_view line);

inline std::string withNewline(std::string text) {
  text.push_back('\n');
  return text;
}

inline std::string okLogin(std::string_view name) {
  std::string out = "OK LOGIN ";
  out.append(name.data(), name.size());
  return withNewline(std::move(out));
}

inline std::string roomCreated(int roomId) {
  return withNewline("ROOM_CREATED " + std::to_string(roomId));
}

inline std::string waitingOpponent() {
  return "WAITING_OPPONENT\n";
}

inline std::string roomJoined(int roomId, char mark) {
  return withNewline("ROOM_JOINED " + std::to_string(roomId) + " " + mark);
}

inline std::string opponentJoined(std::string_view name) {
  std::string out = "OPPONENT_JOINED ";
  out.append(name.data(), name.size());
  return withNewline(std::move(out));
}

inline std::string gameStart(char mark) {
  return withNewline(std::string("GAME_START ") + mark);
}

inline std::string boardMessage(std::string_view renderedBoard) {
  std::string out = "BOARD\n";
  out.append(renderedBoard.data(), renderedBoard.size());
  return out;
}

inline std::string yourTurn() {
  return "YOUR_TURN\n";
}

inline std::string opponentTurn() {
  return "OPPONENT_TURN\n";
}

inline std::string gameOver(std::string_view result) {
  std::string out = "GAME_OVER ";
  out.append(result.data(), result.size());
  return withNewline(std::move(out));
}

inline std::string opponentLeft() {
  return "OPPONENT_LEFT\n";
}

inline std::string okQuit() {
  return "OK QUIT\n";
}

inline std::string pong() {
  return "PONG\n";
}

inline std::string error(std::string_view code, std::string_view detail) {
  std::string out = "ERROR ";
  out.append(code.data(), code.size());
  out.push_back(' ');
  out.append(detail.data(), detail.size());
  return withNewline(std::move(out));
}

}  // namespace proto
}  // namespace minigame
