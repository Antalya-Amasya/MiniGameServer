#pragma once

#include "minigame/AccountService.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace minigame {

struct DbSettings {
  std::string host;
  unsigned port = 3306;
  std::string user;
  std::string password;
  std::string database;
};

// 读取 gitignore 的本地环境文件。已存在的环境变量优先，值不会写进日志。
bool loadDotEnv(const std::string& path);

std::optional<DbSettings> settingsFromEnv(const char* databaseKey);

class MysqlAccounts : public AccountService {
 public:
  explicit MysqlAccounts(DbSettings settings, std::size_t poolSize = 4);
  ~MysqlAccounts() override;

  MysqlAccounts(const MysqlAccounts&) = delete;
  MysqlAccounts& operator=(const MysqlAccounts&) = delete;

  bool connected() const;
  // 关掉空闲连接的 TCP，下一次借出时应重建。测试用，不停止 MySQL 服务。
  bool severIdleConnections();
  bool applySchema();
  bool resetSchema();
  bool ping();
  int countGamesForRoom(int roomId);
  bool inspectUser(const std::string& username, const std::string& password, bool& hashed, bool& lastLoginSet);

  AuthResult registerAccount(const std::string& username, const std::string& password) override;
  AuthResult authenticate(const std::string& username, const std::string& password) override;
  HistoryResult history(std::uint64_t accountId, int limit) override;
  StatsResult stats(std::uint64_t accountId) override;
  bool saveGame(const FinishedRecord& record) override;

 private:
  struct Pool;
  DbSettings settings_;
  Pool* pool_ = nullptr;
};

}  // namespace minigame
