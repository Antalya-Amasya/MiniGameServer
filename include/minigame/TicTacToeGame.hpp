#pragma once

#include <array>
#include <string>

namespace minigame {

// 井字棋规则与网络无关。
// 谁获胜只由这个类判断，Session / Room 不直接扫描棋盘。

enum class Mark { X, O };

enum class Cell { Empty, X, O };

enum class GameState { InProgress, XWin, OWin, Draw };

enum class MoveError {
  Ok,
  OutOfRange,
  CellOccupied,
  WrongTurn,
  GameFinished
};

struct MoveOutcome {
  MoveError error = MoveError::Ok;
  GameState state = GameState::InProgress;
};

class TicTacToeGame {
 public:
  static constexpr int kSize = 3;

  TicTacToeGame();

  Mark currentMark() const;
  GameState state() const;
  bool finished() const;
  Cell cellAt(int row, int col) const;

  // row、col 都是 0..2。调用方必须传入实际出手的一方。
  MoveOutcome tryMove(Mark mark, int row, int col);

  // 三行，每行三个字符，以 '\n' 结尾。空位是 '.'。
  std::string renderBoard() const;

 private:
  bool winsFrom(int row, int col, Cell placed) const;
  static Cell cellOf(Mark mark);
  static char glyph(Cell cell);

  std::array<std::array<Cell, kSize>, kSize> cells_;
  Mark current_ = Mark::X;
  GameState state_ = GameState::InProgress;
  int moveCount_ = 0;
};

const char* resultToken(GameState state);
char markSymbol(Mark mark);

}  // namespace minigame
