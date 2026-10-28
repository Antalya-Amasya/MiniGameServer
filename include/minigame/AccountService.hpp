#pragma once

#include "minigame/Room.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace minigame {

struct AuthResult {
  bool ok = false;
  std::uint64_t accountId = 0;
  std::string code;
  std::string detail;
};

struct HistoryEntry {
  std::uint64_t id = 0;
  std::string result;
  std::string finishedAt;
};

struct HistoryResult {
  bool ok = false;
  std::string code;
  std::string detail;
  std::vector<HistoryEntry> rows;
};

struct StatsResult {
  bool ok = false;
  std::string code;
  std::string detail;
  int games = 0;
  int wins = 0;
  int losses = 0;
  int draws = 0;
};

// 账号和对局持久化。调用方在 worker 线程上使用，不能在 Reactor 线程上调用。
// 实现不得把 SQL 或凭据放进返回给客户端的 detail。
class AccountService {
 public:
  virtual ~AccountService() = default;

  virtual AuthResult registerAccount(const std::string& username, const std::string& password) = 0;
  virtual AuthResult authenticate(const std::string& username, const std::string& password) = 0;
  virtual HistoryResult history(std::uint64_t accountId, int limit) = 0;
  virtual StatsResult stats(std::uint64_t accountId) = 0;
  virtual bool saveGame(const FinishedRecord& record) = 0;
};

}  // namespace minigame
