#include "minigame/TicTacToeGame.hpp"

namespace minigame {

TicTacToeGame::TicTacToeGame() {
  for (int row = 0; row < kSize; ++row) {
    for (int col = 0; col < kSize; ++col) {
      cells_[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)] = Cell::Empty;
    }
  }
}

Mark TicTacToeGame::currentMark() const {
  return current_;
}

GameState TicTacToeGame::state() const {
  return state_;
}

bool TicTacToeGame::finished() const {
  return state_ != GameState::InProgress;
}

Cell TicTacToeGame::cellAt(int row, int col) const {
  if (row < 0 || col < 0 || row >= kSize || col >= kSize) {
    return Cell::Empty;
  }
  return cells_[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)];
}

MoveOutcome TicTacToeGame::tryMove(Mark mark, int row, int col) {
  MoveOutcome outcome;
  outcome.state = state_;
  if (state_ != GameState::InProgress) {
    outcome.error = MoveError::GameFinished;
    return outcome;
  }
  if (row < 0 || col < 0 || row >= kSize || col >= kSize) {
    outcome.error = MoveError::OutOfRange;
    return outcome;
  }
  if (mark != current_) {
    outcome.error = MoveError::WrongTurn;
    return outcome;
  }
  Cell& target = cells_[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)];
  if (target != Cell::Empty) {
    outcome.error = MoveError::CellOccupied;
    return outcome;
  }

  target = cellOf(mark);
  ++moveCount_;
  if (winsFrom(row, col, target)) {
    state_ = (mark == Mark::X) ? GameState::XWin : GameState::OWin;
  } else if (moveCount_ == kSize * kSize) {
    state_ = GameState::Draw;
  } else {
    current_ = (mark == Mark::X) ? Mark::O : Mark::X;
  }

  outcome.error = MoveError::Ok;
  outcome.state = state_;
  return outcome;
}

std::string TicTacToeGame::renderBoard() const {
  std::string out;
  out.reserve(static_cast<std::size_t>(kSize * (kSize + 1)));
  for (int row = 0; row < kSize; ++row) {
    for (int col = 0; col < kSize; ++col) {
      out.push_back(glyph(cellAt(row, col)));
    }
    out.push_back('\n');
  }
  return out;
}

bool TicTacToeGame::winsFrom(int row, int col, Cell placed) const {
  bool rowWin = true;
  for (int c = 0; c < kSize; ++c) {
    if (cellAt(row, c) != placed) {
      rowWin = false;
    }
  }

  bool colWin = true;
  for (int r = 0; r < kSize; ++r) {
    if (cellAt(r, col) != placed) {
      colWin = false;
    }
  }

  bool mainDiagonal = false;
  if (row == col) {
    mainDiagonal = cellAt(0, 0) == placed && cellAt(1, 1) == placed && cellAt(2, 2) == placed;
  }

  bool antiDiagonal = false;
  if (row + col == kSize - 1) {
    antiDiagonal = cellAt(0, 2) == placed && cellAt(1, 1) == placed && cellAt(2, 0) == placed;
  }

  return rowWin || colWin || mainDiagonal || antiDiagonal;
}

Cell TicTacToeGame::cellOf(Mark mark) {
  return mark == Mark::X ? Cell::X : Cell::O;
}

char TicTacToeGame::glyph(Cell cell) {
  switch (cell) {
    case Cell::X:
      return 'X';
    case Cell::O:
      return 'O';
    case Cell::Empty:
      return '.';
  }
  return '.';
}

const char* resultToken(GameState state) {
  switch (state) {
    case GameState::XWin:
      return "X_WIN";
    case GameState::OWin:
      return "O_WIN";
    case GameState::Draw:
      return "DRAW";
    case GameState::InProgress:
      return "IN_PROGRESS";
  }
  return "IN_PROGRESS";
}

char markSymbol(Mark mark) {
  return mark == Mark::X ? 'X' : 'O';
}

}  // namespace minigame
