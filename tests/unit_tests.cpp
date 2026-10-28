#include "minigame/Logger.hpp"
#include "minigame/Protocol.hpp"
#include "minigame/RoomManager.hpp"
#include "minigame/TicTacToeGame.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

int g_passed = 0;
int g_failed = 0;
int g_assertions = 0;

class Failure : public std::runtime_error {
 public:
  explicit Failure(const std::string& message) : std::runtime_error(message) {}
};

void check(bool condition, const char* expression, const char* file, int line) {
  ++g_assertions;
  if (!condition) {
    throw Failure(std::string(file) + ":" + std::to_string(line) + " " + expression);
  }
}

void expectText(const std::string& actual, const std::string& expected, const char* file, int line) {
  ++g_assertions;
  if (actual != expected) {
    throw Failure(std::string(file) + ":" + std::to_string(line) + "\n expected [" + expected + "]\n actual   [" +
                  actual + "]");
  }
}

#define CHECK(condition) ::check(static_cast<bool>(condition), #condition, __FILE__, __LINE__)
#define EXPECT_TEXT(actual, expected) ::expectText((actual), (expected), __FILE__, __LINE__)

using minigame::Cell;
using minigame::GameState;
using minigame::Mark;
using minigame::MoveError;
using minigame::MoveOutcome;
using minigame::PlayerLink;
using minigame::RoomManager;
using minigame::TicTacToeGame;

class FakePlayer final : public PlayerLink {
 public:
  FakePlayer(std::uint64_t playerId, std::string playerName)
      : id_(playerId), name_(std::move(playerName)) {}

  std::uint64_t id() const override { return id_; }
  const std::string& name() const override { return name_; }
  void send(std::string_view text) override { inbox.append(text.data(), text.size()); }
  void onRoomChanged(int newRoomId) override { roomId_ = newRoomId; }

  int roomId() const { return roomId_; }

  std::string inbox;

 private:
  std::uint64_t id_ = 0;
  std::string name_;
  int roomId_ = 0;
};

MoveOutcome play(TicTacToeGame& game, Mark mark, int row, int col) {
  const MoveOutcome outcome = game.tryMove(mark, row, col);
  CHECK(outcome.error == MoveError::Ok);
  return outcome;
}

void testInitialBoardIsEmpty() {
  TicTacToeGame game;
  EXPECT_TEXT(game.renderBoard(), "...\n...\n...\n");
  CHECK(game.state() == GameState::InProgress);
  CHECK(!game.finished());
  for (int row = 0; row < TicTacToeGame::kSize; ++row) {
    for (int col = 0; col < TicTacToeGame::kSize; ++col) {
      CHECK(game.cellAt(row, col) == Cell::Empty);
    }
  }
}

void testXMovesFirst() {
  const TicTacToeGame game;
  CHECK(game.currentMark() == Mark::X);
}

void testLegalMovePlacesMarkAndSwitchesTurn() {
  TicTacToeGame game;
  const MoveOutcome outcome = play(game, Mark::X, 1, 1);
  CHECK(outcome.state == GameState::InProgress);
  CHECK(game.cellAt(1, 1) == Cell::X);
  CHECK(game.currentMark() == Mark::O);
  EXPECT_TEXT(game.renderBoard(), "...\n.X.\n...\n");
}

void testOccupiedCellIsRejected() {
  TicTacToeGame game;
  play(game, Mark::X, 0, 0);
  const MoveOutcome outcome = game.tryMove(Mark::O, 0, 0);
  CHECK(outcome.error == MoveError::CellOccupied);
  CHECK(outcome.state == GameState::InProgress);
  CHECK(game.currentMark() == Mark::O);
  CHECK(game.cellAt(0, 0) == Cell::X);
  EXPECT_TEXT(game.renderBoard(), "X..\n...\n...\n");
}

void testOutOfRangeIsRejected() {
  TicTacToeGame game;
  const MoveOutcome negative = game.tryMove(Mark::X, -1, 0);
  const MoveOutcome tooBig = game.tryMove(Mark::X, 0, 3);
  CHECK(negative.error == MoveError::OutOfRange);
  CHECK(tooBig.error == MoveError::OutOfRange);
  CHECK(game.currentMark() == Mark::X);
  EXPECT_TEXT(game.renderBoard(), "...\n...\n...\n");
}

void testWrongTurnIsRejected() {
  TicTacToeGame game;
  const MoveOutcome outcome = game.tryMove(Mark::O, 0, 0);
  CHECK(outcome.error == MoveError::WrongTurn);
  CHECK(game.currentMark() == Mark::X);
  CHECK(game.cellAt(0, 0) == Cell::Empty);

  play(game, Mark::X, 0, 0);
  const MoveOutcome again = game.tryMove(Mark::X, 0, 1);
  CHECK(again.error == MoveError::WrongTurn);
  CHECK(game.currentMark() == Mark::O);
  CHECK(game.cellAt(0, 1) == Cell::Empty);
}

void testHorizontalWin() {
  TicTacToeGame game;
  play(game, Mark::X, 0, 0);
  play(game, Mark::O, 1, 0);
  play(game, Mark::X, 0, 1);
  play(game, Mark::O, 1, 1);
  const MoveOutcome outcome = play(game, Mark::X, 0, 2);
  CHECK(outcome.state == GameState::XWin);
  CHECK(game.finished());
  EXPECT_TEXT(game.renderBoard(), "XXX\nOO.\n...\n");
}

void testVerticalWin() {
  TicTacToeGame game;
  play(game, Mark::X, 0, 0);
  play(game, Mark::O, 0, 2);
  play(game, Mark::X, 1, 0);
  play(game, Mark::O, 1, 2);
  play(game, Mark::X, 0, 1);
  const MoveOutcome outcome = play(game, Mark::O, 2, 2);
  CHECK(outcome.state == GameState::OWin);
  EXPECT_TEXT(game.renderBoard(), "XXO\nX.O\n..O\n");
}

void testMainDiagonalWin() {
  TicTacToeGame game;
  play(game, Mark::X, 0, 0);
  play(game, Mark::O, 0, 1);
  play(game, Mark::X, 1, 1);
  play(game, Mark::O, 0, 2);
  const MoveOutcome outcome = play(game, Mark::X, 2, 2);
  CHECK(outcome.state == GameState::XWin);
  EXPECT_TEXT(game.renderBoard(), "XOO\n.X.\n..X\n");
}

void testAntiDiagonalWin() {
  TicTacToeGame game;
  play(game, Mark::X, 0, 2);
  play(game, Mark::O, 0, 0);
  play(game, Mark::X, 1, 1);
  play(game, Mark::O, 1, 0);
  const MoveOutcome outcome = play(game, Mark::X, 2, 0);
  CHECK(outcome.state == GameState::XWin);
  EXPECT_TEXT(game.renderBoard(), "O.X\nOX.\nX..\n");
}

void testDraw() {
  TicTacToeGame game;
  play(game, Mark::X, 0, 0);
  play(game, Mark::O, 0, 1);
  play(game, Mark::X, 0, 2);
  play(game, Mark::O, 1, 1);
  play(game, Mark::X, 1, 0);
  play(game, Mark::O, 1, 2);
  play(game, Mark::X, 2, 1);
  play(game, Mark::O, 2, 0);
  CHECK(game.state() == GameState::InProgress);
  const MoveOutcome outcome = play(game, Mark::X, 2, 2);
  CHECK(outcome.state == GameState::Draw);
  CHECK(game.finished());
  EXPECT_TEXT(game.renderBoard(), "XOX\nXOO\nOXX\n");
}

void testNoMoveAfterGameOver() {
  TicTacToeGame game;
  play(game, Mark::X, 0, 0);
  play(game, Mark::O, 1, 0);
  play(game, Mark::X, 0, 1);
  play(game, Mark::O, 1, 1);
  play(game, Mark::X, 0, 2);
  const MoveOutcome byWinner = game.tryMove(Mark::X, 2, 2);
  const MoveOutcome byLoser = game.tryMove(Mark::O, 2, 2);
  CHECK(byWinner.error == MoveError::GameFinished);
  CHECK(byLoser.error == MoveError::GameFinished);
  CHECK(game.state() == GameState::XWin);
  EXPECT_TEXT(game.renderBoard(), "XXX\nOO.\n...\n");
}

void testParseValidCommands() {
  const minigame::proto::Command login = minigame::proto::parseCommand("LOGIN alice");
  CHECK(login.type == minigame::proto::CommandType::Login);
  EXPECT_TEXT(login.name, "alice");

  const minigame::proto::Command passwordLogin = minigame::proto::parseCommand("LOGIN alice extra");
  CHECK(passwordLogin.type == minigame::proto::CommandType::Login);
  EXPECT_TEXT(passwordLogin.password, "extra");

  const minigame::proto::Command created = minigame::proto::parseCommand("CREATE_ROOM");
  CHECK(created.type == minigame::proto::CommandType::CreateRoom);

  const minigame::proto::Command joined = minigame::proto::parseCommand("JOIN_ROOM 1001");
  CHECK(joined.type == minigame::proto::CommandType::JoinRoom);
  CHECK(joined.roomId == 1001);

  const minigame::proto::Command move = minigame::proto::parseCommand("MOVE  1\t2");
  CHECK(move.type == minigame::proto::CommandType::Move);
  CHECK(move.row == 1);
  CHECK(move.col == 2);

  const minigame::proto::Command quit = minigame::proto::parseCommand("QUIT");
  CHECK(quit.type == minigame::proto::CommandType::Quit);
}

void testParseRejectsMalformedCommands() {
  CHECK(minigame::proto::parseCommand("").type == minigame::proto::CommandType::Invalid);
  CHECK(minigame::proto::parseCommand("login alice").type == minigame::proto::CommandType::Invalid);
  CHECK(minigame::proto::parseCommand("LOGIN").type == minigame::proto::CommandType::Invalid);
  CHECK(minigame::proto::parseCommand("LOGIN alice extra more").type == minigame::proto::CommandType::Invalid);
  CHECK(minigame::proto::parseCommand("MOVE -1 0").type == minigame::proto::CommandType::Invalid);
  CHECK(minigame::proto::parseCommand("MOVE 1").type == minigame::proto::CommandType::Invalid);
  CHECK(minigame::proto::parseCommand("JOIN_ROOM room").type == minigame::proto::CommandType::Invalid);
  CHECK(minigame::proto::parseCommand("NOPE").type == minigame::proto::CommandType::Invalid);
  CHECK(minigame::proto::isValidPlayerName("alice"));
  CHECK(minigame::proto::isValidPlayerName("_A1"));
  CHECK(!minigame::proto::isValidPlayerName(""));
  CHECK(!minigame::proto::isValidPlayerName("alice!"));
  CHECK(!minigame::proto::isValidPlayerName("abcdefghijklmnopq"));
}

void loginTwo(RoomManager& rooms, const std::shared_ptr<FakePlayer>& first, const std::shared_ptr<FakePlayer>& second) {
  const minigame::LoginStatus firstLogin = rooms.login(first->id(), first->name());
  const minigame::LoginStatus secondLogin = rooms.login(second->id(), second->name());
  CHECK(firstLogin.ok);
  CHECK(secondLogin.ok);
}

void testCreateAndJoinStartsGame() {
  RoomManager rooms;
  std::shared_ptr<FakePlayer> alice = std::make_shared<FakePlayer>(1, "alice");
  std::shared_ptr<FakePlayer> bob = std::make_shared<FakePlayer>(2, "bob");
  loginTwo(rooms, alice, bob);

  const minigame::CreateRoomStatus created = rooms.createRoom(alice);
  CHECK(created.ok);
  CHECK(created.roomId == 1001);
  CHECK(alice->roomId() == 1001);
  CHECK(alice->inbox.find("ROOM_CREATED 1001\n") != std::string::npos);
  CHECK(alice->inbox.find("WAITING_OPPONENT\n") != std::string::npos);
  CHECK(rooms.roomCount() == 1);

  const minigame::MoveStatus tooEarly = rooms.move(alice->id(), 0, 0);
  CHECK(!tooEarly.ok);
  EXPECT_TEXT(tooEarly.code, minigame::proto::kGameNotStarted);

  const minigame::JoinRoomStatus joined = rooms.joinRoom(created.roomId, bob);
  CHECK(joined.ok);
  CHECK(joined.mark == 'O');
  CHECK(bob->roomId() == 1001);
  CHECK(alice->inbox.find("OPPONENT_JOINED bob\n") != std::string::npos);
  CHECK(alice->inbox.find("GAME_START X\n") != std::string::npos);
  CHECK(alice->inbox.find("YOUR_TURN\n") != std::string::npos);
  CHECK(bob->inbox.find("ROOM_JOINED 1001 O\n") != std::string::npos);
  CHECK(bob->inbox.find("GAME_START O\n") != std::string::npos);
  CHECK(bob->inbox.find("OPPONENT_TURN\n") != std::string::npos);
}

void testRoomRejectsMissingAndFull() {
  RoomManager rooms;
  std::shared_ptr<FakePlayer> alice = std::make_shared<FakePlayer>(1, "alice");
  std::shared_ptr<FakePlayer> bob = std::make_shared<FakePlayer>(2, "bob");
  std::shared_ptr<FakePlayer> carol = std::make_shared<FakePlayer>(3, "carol");
  CHECK(rooms.login(alice->id(), alice->name()).ok);
  CHECK(rooms.login(bob->id(), bob->name()).ok);
  CHECK(rooms.login(carol->id(), carol->name()).ok);

  const minigame::JoinRoomStatus missing = rooms.joinRoom(1001, bob);
  CHECK(!missing.ok);
  EXPECT_TEXT(missing.code, minigame::proto::kRoomNotFound);

  CHECK(rooms.createRoom(alice).ok);
  CHECK(rooms.joinRoom(1001, bob).ok);
  const minigame::JoinRoomStatus full = rooms.joinRoom(1001, carol);
  CHECK(!full.ok);
  EXPECT_TEXT(full.code, minigame::proto::kRoomFull);
  CHECK(carol->roomId() == 0);

  const minigame::CreateRoomStatus busy = rooms.createRoom(alice);
  CHECK(!busy.ok);
  EXPECT_TEXT(busy.code, minigame::proto::kAlreadyInRoom);
}

void testIllegalMovesDoNotBroadcast() {
  RoomManager rooms;
  std::shared_ptr<FakePlayer> alice = std::make_shared<FakePlayer>(1, "alice");
  std::shared_ptr<FakePlayer> bob = std::make_shared<FakePlayer>(2, "bob");
  loginTwo(rooms, alice, bob);
  CHECK(rooms.createRoom(alice).ok);
  CHECK(rooms.joinRoom(1001, bob).ok);
  alice->inbox.clear();
  bob->inbox.clear();

  const minigame::MoveStatus wrongTurn = rooms.move(bob->id(), 1, 1);
  CHECK(!wrongTurn.ok);
  CHECK(wrongTurn.error == MoveError::WrongTurn);
  EXPECT_TEXT(wrongTurn.code, minigame::proto::kNotYourTurn);
  CHECK(alice->inbox.empty());
  CHECK(bob->inbox.empty());

  CHECK(rooms.move(alice->id(), 0, 0).ok);
  alice->inbox.clear();
  bob->inbox.clear();

  const minigame::MoveStatus occupied = rooms.move(bob->id(), 0, 0);
  CHECK(occupied.error == MoveError::CellOccupied);
  CHECK(alice->inbox.empty());

  const minigame::MoveStatus outOfRange = rooms.move(bob->id(), 3, 0);
  CHECK(outOfRange.error == MoveError::OutOfRange);
  CHECK(bob->inbox.empty());
}

void testLeaveRemovesRoomAndNotifiesPartner() {
  RoomManager rooms;
  std::shared_ptr<FakePlayer> alice = std::make_shared<FakePlayer>(1, "alice");
  std::shared_ptr<FakePlayer> bob = std::make_shared<FakePlayer>(2, "bob");
  loginTwo(rooms, alice, bob);
  CHECK(rooms.createRoom(alice).ok);
  CHECK(rooms.joinRoom(1001, bob).ok);

  bob->inbox.clear();
  rooms.logout(alice->id());
  CHECK(rooms.roomCount() == 0);
  CHECK(alice->roomId() == 0);
  CHECK(bob->roomId() == 0);
  CHECK(rooms.loggedInCount() == 1);
  CHECK(bob->inbox.find("OPPONENT_LEFT\n") != std::string::npos);
  CHECK(bob->inbox.find("GAME_OVER OPPONENT_LEFT\n") != std::string::npos);

  const minigame::JoinRoomStatus gone = rooms.joinRoom(1001, bob);
  CHECK(!gone.ok);
  EXPECT_TEXT(gone.code, minigame::proto::kRoomNotFound);

  const minigame::CreateRoomStatus next = rooms.createRoom(bob);
  CHECK(next.ok);
  CHECK(next.roomId == 1002);
  CHECK(rooms.roomCount() == 1);
}

void testGameOverThenFurtherMoveAndCleanup() {
  RoomManager rooms;
  std::shared_ptr<FakePlayer> alice = std::make_shared<FakePlayer>(1, "alice");
  std::shared_ptr<FakePlayer> bob = std::make_shared<FakePlayer>(2, "bob");
  loginTwo(rooms, alice, bob);
  CHECK(rooms.createRoom(alice).ok);
  CHECK(rooms.joinRoom(1001, bob).ok);

  CHECK(rooms.move(alice->id(), 0, 0).ok);
  CHECK(rooms.move(bob->id(), 1, 0).ok);
  CHECK(rooms.move(alice->id(), 0, 1).ok);
  CHECK(rooms.move(bob->id(), 1, 1).ok);
  const minigame::MoveStatus winning = rooms.move(alice->id(), 0, 2);
  CHECK(winning.ok);
  CHECK(alice->inbox.find("XXX\n") != std::string::npos);
  CHECK(alice->inbox.find("GAME_OVER X_WIN\n") != std::string::npos);
  CHECK(bob->inbox.find("GAME_OVER X_WIN\n") != std::string::npos);
  CHECK(rooms.roomCount() == 1);

  alice->inbox.clear();
  const minigame::MoveStatus afterEnd = rooms.move(alice->id(), 2, 2);
  CHECK(!afterEnd.ok);
  CHECK(afterEnd.error == MoveError::GameFinished);
  EXPECT_TEXT(afterEnd.code, minigame::proto::kGameOver);
  CHECK(alice->inbox.empty());

  const minigame::CreateRoomStatus stillInside = rooms.createRoom(alice);
  CHECK(!stillInside.ok);
  EXPECT_TEXT(stillInside.code, minigame::proto::kAlreadyInRoom);

  rooms.logout(alice->id());
  rooms.logout(bob->id());
  CHECK(rooms.roomCount() == 0);
  CHECK(rooms.loggedInCount() == 0);
}

void testLoginNames() {
  RoomManager rooms;
  CHECK(!rooms.login(1, "bad name").ok);
  CHECK(!rooms.login(1, "alice!").ok);
  CHECK(rooms.login(1, "alice").ok);
  const minigame::LoginStatus again = rooms.login(1, "alice2");
  CHECK(!again.ok);
  EXPECT_TEXT(again.code, minigame::proto::kAlreadyLoggedIn);
  const minigame::LoginStatus taken = rooms.login(2, "alice");
  CHECK(!taken.ok);
  EXPECT_TEXT(taken.code, minigame::proto::kNameTaken);

  std::shared_ptr<FakePlayer> stranger = std::make_shared<FakePlayer>(9, "stranger");
  const minigame::CreateRoomStatus denied = rooms.createRoom(stranger);
  CHECK(!denied.ok);
  EXPECT_TEXT(denied.code, minigame::proto::kNotLoggedIn);
}

void runCase(const char* name, void (*function)()) {
  try {
    function();
    std::cout << "PASS " << name << "\n";
    ++g_passed;
  } catch (const Failure& failure) {
    std::cout << "FAIL " << name << " " << failure.what() << "\n";
    ++g_failed;
  } catch (const std::exception& ex) {
    std::cout << "FAIL " << name << " exception " << ex.what() << "\n";
    ++g_failed;
  }
}

}  // namespace

int main() {
  minigame::log::setEnabled(false);
  runCase("initial_board_is_empty", testInitialBoardIsEmpty);
  runCase("x_moves_first", testXMovesFirst);
  runCase("legal_move", testLegalMovePlacesMarkAndSwitchesTurn);
  runCase("occupied_cell", testOccupiedCellIsRejected);
  runCase("out_of_range", testOutOfRangeIsRejected);
  runCase("wrong_turn", testWrongTurnIsRejected);
  runCase("horizontal_win", testHorizontalWin);
  runCase("vertical_win", testVerticalWin);
  runCase("main_diagonal_win", testMainDiagonalWin);
  runCase("anti_diagonal_win", testAntiDiagonalWin);
  runCase("draw", testDraw);
  runCase("no_move_after_game_over", testNoMoveAfterGameOver);
  runCase("parse_valid_commands", testParseValidCommands);
  runCase("parse_malformed_commands", testParseRejectsMalformedCommands);
  runCase("create_and_join", testCreateAndJoinStartsGame);
  runCase("room_missing_and_full", testRoomRejectsMissingAndFull);
  runCase("illegal_moves_do_not_broadcast", testIllegalMovesDoNotBroadcast);
  runCase("leave_removes_room", testLeaveRemovesRoomAndNotifiesPartner);
  runCase("game_over_then_cleanup", testGameOverThenFurtherMoveAndCleanup);
  runCase("login_names", testLoginNames);

  std::cout << "UNIT_SUMMARY tests=" << (g_passed + g_failed) << " passed=" << g_passed
            << " failed=" << g_failed << " assertions=" << g_assertions << "\n";
  return g_failed == 0 ? 0 : 1;
}
