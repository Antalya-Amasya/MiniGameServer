#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "minigame/Database.hpp"

#include "minigame/Logger.hpp"
#include "minigame/Protocol.hpp"

#include <mysql.h>

#include <crypt.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <memory>

#include <sys/socket.h>
#include <mutex>
#include <vector>

namespace minigame {
namespace {

constexpr int kDuplicateKey = 1062;

const char* kSchema[] = {
    "CREATE TABLE IF NOT EXISTS users ("
    " id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
    " username VARCHAR(32) NOT NULL,"
    " password_hash VARCHAR(100) NOT NULL,"
    " created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    " last_login_at TIMESTAMP NULL DEFAULT NULL,"
    " PRIMARY KEY (id),"
    " UNIQUE KEY users_username (username)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
    "CREATE TABLE IF NOT EXISTS game_records ("
    " id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
    " room_id INT NOT NULL,"
    " player_x_id BIGINT UNSIGNED NOT NULL,"
    " player_o_id BIGINT UNSIGNED NOT NULL,"
    " winner CHAR(1) NULL,"
    " result VARCHAR(32) NOT NULL,"
    " started_at TIMESTAMP NOT NULL,"
    " finished_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    " PRIMARY KEY (id),"
    " UNIQUE KEY game_once (room_id, started_at),"
    " KEY game_player_x (player_x_id),"
    " KEY game_player_o (player_o_id),"
    " KEY game_finished (finished_at)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
};

std::string trimCr(std::string value) {
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
    value.pop_back();
  }
  return value;
}

void logDb(const char* what, const char* detail) {
  std::string message = what;
  if (detail != nullptr && detail[0] != '\0') {
    message += ": ";
    message += detail;
  }
  log::error(message);
}

bool hashPassword(const std::string& password, std::string& out) {
  char salt[64];
  if (crypt_gensalt_rn("$2b$", 12, nullptr, 0, salt, static_cast<int>(sizeof(salt))) == nullptr) {
    logDb("bcrypt salt", "unavailable");
    return false;
  }
  auto data = std::make_unique<crypt_data>();
  data->initialized = 0;
  const char* hashed = crypt_r(password.c_str(), salt, data.get());
  if (hashed == nullptr) {
    logDb("bcrypt hash", "unavailable");
    return false;
  }
  out = hashed;
  return out.size() > 4 && out.compare(0, 2, "$2") == 0;
}

bool verifyPassword(const std::string& password, const std::string& stored) {
  if (stored.compare(0, 2, "$2") != 0) {
    return false;
  }
  auto data = std::make_unique<crypt_data>();
  data->initialized = 0;
  const char* hashed = crypt_r(password.c_str(), stored.c_str(), data.get());
  return hashed != nullptr && stored == hashed;
}

struct Statement {
  MYSQL_STMT* handle = nullptr;
  explicit Statement(MYSQL* conn) : handle(mysql_stmt_init(conn)) {}
  ~Statement() {
    if (handle != nullptr) {
      mysql_stmt_close(handle);
    }
  }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
};

void bindString(MYSQL_BIND& param, const std::string& value, unsigned long& length) {
  param.buffer_type = MYSQL_TYPE_STRING;
  param.buffer = const_cast<char*>(value.data());
  param.buffer_length = static_cast<unsigned long>(value.size());
  length = static_cast<unsigned long>(value.size());
  param.length = &length;
}

AuthResult unavailable() {
  AuthResult result;
  result.code = proto::kDatabaseUnavailable;
  result.detail = "database unavailable";
  return result;
}

}  // namespace

struct MysqlAccounts::Pool {
  struct Lease {
    Pool* pool = nullptr;
    MYSQL* conn = nullptr;
    MYSQL* get() const { return conn; }
    Lease() = default;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept : pool(other.pool), conn(other.conn) {
      other.pool = nullptr;
      other.conn = nullptr;
    }
    Lease& operator=(Lease&& other) noexcept {
      if (this != &other) {
        release();
        pool = other.pool;
        conn = other.conn;
        other.pool = nullptr;
        other.conn = nullptr;
      }
      return *this;
    }
    ~Lease() { release(); }
    void release() {
      if (pool != nullptr && conn != nullptr) {
        pool->giveBack(conn);
      }
      pool = nullptr;
      conn = nullptr;
    }
  };

  explicit Pool(DbSettings settings, std::size_t size) : settings_(std::move(settings)), targetSize_(size == 0 ? 1 : size) {
    idle_.reserve(targetSize_);
    for (std::size_t index = 0; index < targetSize_; ++index) {
      if (MYSQL* conn = open()) {
        idle_.push_back(conn);
      }
    }
  }

  ~Pool() { shutdown(); }

  bool usable() const {
    std::lock_guard<std::mutex> lock(mu_);
    return !idle_.empty() || checkedOut_ > 0;
  }

  Lease acquire() {
    MYSQL* existing = nullptr;
    bool makeNew = false;
    {
      std::unique_lock<std::mutex> lock(mu_);
      if (!cv_.wait_for(lock, std::chrono::seconds(2), [&] {
            return stopping_ || !idle_.empty() || checkedOut_ < static_cast<int>(targetSize_);
          })) {
        return {};
      }
      if (stopping_) {
        return {};
      }
      if (!idle_.empty()) {
        existing = idle_.back();
        idle_.pop_back();
        ++checkedOut_;
      } else if (checkedOut_ < static_cast<int>(targetSize_)) {
        ++checkedOut_;
        makeNew = true;
      } else {
        return {};
      }
    }

    MYSQL* conn = makeNew ? openFresh() : ensureLive(existing);
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (conn == nullptr || stopping_) {
        if (conn != nullptr) {
          mysql_close(conn);
        }
        if (checkedOut_ > 0) {
          --checkedOut_;
        }
        cv_.notify_all();
        return {};
      }
    }
    Lease lease;
    lease.pool = this;
    lease.conn = conn;
    return lease;
  }

  bool severIdle() {
    std::vector<MYSQL*> snapshot;
    {
      std::lock_guard<std::mutex> lock(mu_);
      snapshot = idle_;
    }
    if (snapshot.empty()) {
      return false;
    }
    for (MYSQL* conn : snapshot) {
      const int fd = static_cast<int>(conn->net.fd);
      if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
      }
    }
    return true;
  }

  void shutdown() {
    std::vector<MYSQL*> closing;
    {
      std::unique_lock<std::mutex> lock(mu_);
      stopping_ = true;
      cv_.notify_all();
      cv_.wait_for(lock, std::chrono::seconds(3), [&] { return checkedOut_ == 0; });
      closing.swap(idle_);
    }
    for (MYSQL* conn : closing) {
      mysql_close(conn);
    }
  }

  void giveBack(MYSQL* conn) {
    std::lock_guard<std::mutex> lock(mu_);
    if (checkedOut_ > 0) {
      --checkedOut_;
    }
    if (stopping_) {
      mysql_close(conn);
    } else {
      idle_.push_back(conn);
    }
    cv_.notify_all();
  }

  MYSQL* open() const {
    MYSQL* conn = mysql_init(nullptr);
    if (conn == nullptr) {
      logDb("mysql_init", "failed");
      return nullptr;
    }
    unsigned int timeout = 2;
    bool autoReconnect = false;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(conn, MYSQL_OPT_RECONNECT, &autoReconnect);
    mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if (mysql_real_connect(conn, settings_.host.c_str(), settings_.user.c_str(), settings_.password.c_str(),
                           settings_.database.c_str(), settings_.port, nullptr, 0) == nullptr) {
      logDb("mysql connect", mysql_error(conn));
      mysql_close(conn);
      return nullptr;
    }
    return conn;
  }

  MYSQL* openFresh() const {
    for (int attempt = 0; attempt < 3; ++attempt) {
      MYSQL* conn = open();
      if (conn != nullptr && mysql_ping(conn) == 0) {
        return conn;
      }
      if (conn != nullptr) {
        mysql_close(conn);
      }
    }
    logDb("mysql reconnect", "gave up after 3 attempts");
    return nullptr;
  }

  MYSQL* ensureLive(MYSQL* conn) const {
    for (int attempt = 0; attempt < 3; ++attempt) {
      if (conn != nullptr && mysql_ping(conn) == 0) {
        return conn;
      }
      if (conn != nullptr) {
        mysql_close(conn);
        conn = nullptr;
      }
      conn = open();
    }
    if (conn != nullptr && mysql_ping(conn) == 0) {
      return conn;
    }
    if (conn != nullptr) {
      mysql_close(conn);
    }
    logDb("mysql reconnect", "gave up after 3 attempts");
    return nullptr;
  }

  DbSettings settings_;
  std::size_t targetSize_ = 1;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<MYSQL*> idle_;
  int checkedOut_ = 0;
  bool stopping_ = false;
};

bool loadDotEnv(const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    return false;
  }
  std::string line;
  while (std::getline(input, line)) {
    line = trimCr(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const std::size_t split = line.find('=');
    if (split == std::string::npos || split == 0) {
      continue;
    }
    const std::string key = line.substr(0, split);
    const std::string value = trimCr(line.substr(split + 1));
    if (std::getenv(key.c_str()) == nullptr) {
      setenv(key.c_str(), value.c_str(), 0);
    }
  }
  return true;
}

std::optional<DbSettings> settingsFromEnv(const char* databaseKey) {
  const char* host = std::getenv("MINIGAME_DB_HOST");
  const char* portText = std::getenv("MINIGAME_DB_PORT");
  const char* user = std::getenv("MINIGAME_DB_USER");
  const char* password = std::getenv("MINIGAME_DB_PASSWORD");
  const char* database = std::getenv(databaseKey);
  if (host == nullptr || portText == nullptr || user == nullptr || password == nullptr || database == nullptr) {
    return std::nullopt;
  }
  DbSettings settings;
  settings.host = trimCr(host);
  settings.user = trimCr(user);
  settings.password = trimCr(password);
  settings.database = trimCr(database);
  int port = 0;
  for (char ch : trimCr(portText)) {
    if (ch < '0' || ch > '9') {
      return std::nullopt;
    }
    port = port * 10 + (ch - '0');
  }
  if (port <= 0 || port > 65535 || settings.host.empty() || settings.database.empty()) {
    return std::nullopt;
  }
  settings.port = static_cast<unsigned>(port);
  return settings;
}

MysqlAccounts::MysqlAccounts(DbSettings settings, std::size_t poolSize)
    : settings_(std::move(settings)), pool_(new Pool(settings_, poolSize == 0 ? 1 : poolSize)) {}

MysqlAccounts::~MysqlAccounts() {
  delete pool_;
  pool_ = nullptr;
}

bool MysqlAccounts::connected() const {
  return pool_ != nullptr && pool_->usable();
}

bool MysqlAccounts::severIdleConnections() {
  return pool_ != nullptr && pool_->severIdle();
}

bool MysqlAccounts::applySchema() {
  Pool::Lease lease = pool_->acquire();
  MYSQL* conn = lease.get();
  if (conn == nullptr) {
    return false;
  }
  for (const char* statement : kSchema) {
    if (mysql_query(conn, statement) != 0) {
      logDb("schema", mysql_error(conn));
      return false;
    }
  }
  return true;
}

bool MysqlAccounts::resetSchema() {
  Pool::Lease lease = pool_->acquire();
  MYSQL* conn = lease.get();
  if (conn == nullptr) {
    return false;
  }
  if (mysql_query(conn, "DROP TABLE IF EXISTS game_records") != 0 ||
      mysql_query(conn, "DROP TABLE IF EXISTS users") != 0) {
    logDb("reset schema", mysql_error(conn));
    return false;
  }
  lease.release();
  return applySchema();
}

bool MysqlAccounts::ping() {
  Pool::Lease lease = pool_->acquire();
  MYSQL* conn = lease.get();
  if (conn == nullptr) {
    return false;
  }
  if (mysql_query(conn, "SELECT 1") != 0) {
    return false;
  }
  MYSQL_RES* result = mysql_store_result(conn);
  if (result == nullptr) {
    return false;
  }
  mysql_free_result(result);
  return true;
}

AuthResult MysqlAccounts::registerAccount(const std::string& username, const std::string& password) {
  if (!proto::isValidPlayerName(username) || !proto::isValidPassword(password)) {
    AuthResult result;
    result.code = proto::kInvalidCredentials;
    result.detail = "invalid username or password";
    return result;
  }
  std::string hash;
  if (!hashPassword(password, hash)) {
    AuthResult result;
    result.code = proto::kInternalServerError;
    result.detail = "password hash failed";
    return result;
  }
  Pool::Lease lease = pool_->acquire();
  MYSQL* conn = lease.get();
  if (conn == nullptr) {
    return unavailable();
  }
  Statement statement(conn);
  const char* sql = "INSERT INTO users (username, password_hash) VALUES (?, ?)";
  if (statement.handle == nullptr || mysql_stmt_prepare(statement.handle, sql, std::strlen(sql)) != 0) {
    logDb("register prepare", statement.handle == nullptr ? "no statement" : mysql_stmt_error(statement.handle));
    return unavailable();
  }
  MYSQL_BIND params[2]{};
  unsigned long nameLen = 0;
  unsigned long hashLen = 0;
  bindString(params[0], username, nameLen);
  bindString(params[1], hash, hashLen);
  if (mysql_stmt_bind_param(statement.handle, params) != 0 || mysql_stmt_execute(statement.handle) != 0) {
    if (statement.handle != nullptr && mysql_stmt_errno(statement.handle) == kDuplicateKey) {
      AuthResult result;
      result.code = proto::kUserExists;
      result.detail = "username already exists";
      return result;
    }
    logDb("register", mysql_stmt_error(statement.handle));
    return unavailable();
  }
  AuthResult result;
  result.ok = true;
  result.accountId = mysql_stmt_insert_id(statement.handle);
  return result;
}

AuthResult MysqlAccounts::authenticate(const std::string& username, const std::string& password) {
  if (!proto::isValidPlayerName(username) || !proto::isValidPassword(password)) {
    AuthResult result;
    result.code = proto::kInvalidCredentials;
    result.detail = "invalid username or password";
    return result;
  }
  Pool::Lease lease = pool_->acquire();
  MYSQL* conn = lease.get();
  if (conn == nullptr) {
    return unavailable();
  }
  Statement statement(conn);
  const char* sql = "SELECT id, password_hash FROM users WHERE username = ? LIMIT 1";
  if (statement.handle == nullptr || mysql_stmt_prepare(statement.handle, sql, std::strlen(sql)) != 0) {
    logDb("login prepare", statement.handle == nullptr ? "no statement" : mysql_stmt_error(statement.handle));
    return unavailable();
  }
  MYSQL_BIND param{};
  unsigned long nameLen = 0;
  bindString(param, username, nameLen);
  unsigned long long accountId = 0;
  char hashBuffer[128];
  unsigned long hashLen = 0;
  MYSQL_BIND result[2]{};
  result[0].buffer_type = MYSQL_TYPE_LONGLONG;
  result[0].buffer = &accountId;
  result[0].is_unsigned = 1;
  result[1].buffer_type = MYSQL_TYPE_STRING;
  result[1].buffer = hashBuffer;
  result[1].buffer_length = sizeof(hashBuffer);
  result[1].length = &hashLen;
  if (mysql_stmt_bind_param(statement.handle, &param) != 0 || mysql_stmt_bind_result(statement.handle, result) != 0 ||
      mysql_stmt_execute(statement.handle) != 0 || mysql_stmt_store_result(statement.handle) != 0) {
    logDb("login", mysql_stmt_error(statement.handle));
    return unavailable();
  }
  const int fetched = mysql_stmt_fetch(statement.handle);
  if (fetched == MYSQL_NO_DATA) {
    AuthResult denied;
    denied.code = proto::kInvalidCredentials;
    denied.detail = "invalid username or password";
    return denied;
  }
  if (fetched != 0) {
    logDb("login fetch", mysql_stmt_error(statement.handle));
    return unavailable();
  }
  const std::string stored(hashBuffer, hashLen);
  if (!verifyPassword(password, stored)) {
    AuthResult denied;
    denied.code = proto::kInvalidCredentials;
    denied.detail = "invalid username or password";
    return denied;
  }
  mysql_stmt_free_result(statement.handle);
  Statement update(conn);
  const char* updateSql = "UPDATE users SET last_login_at = UTC_TIMESTAMP() WHERE id = ?";
  if (update.handle == nullptr || mysql_stmt_prepare(update.handle, updateSql, std::strlen(updateSql)) != 0) {
    logDb("last login prepare", update.handle == nullptr ? "no statement" : mysql_stmt_error(update.handle));
    return unavailable();
  }
  MYSQL_BIND idParam{};
  idParam.buffer_type = MYSQL_TYPE_LONGLONG;
  idParam.buffer = &accountId;
  idParam.is_unsigned = 1;
  if (mysql_stmt_bind_param(update.handle, &idParam) != 0 || mysql_stmt_execute(update.handle) != 0) {
    logDb("last login", mysql_stmt_error(update.handle));
    return unavailable();
  }
  AuthResult ok;
  ok.ok = true;
  ok.accountId = accountId;
  return ok;
}

bool MysqlAccounts::saveGame(const FinishedRecord& record) {
  if (record.playerX == 0 || record.playerO == 0 || record.startedAt == 0) {
    return false;
  }
  Pool::Lease lease = pool_->acquire();
  MYSQL* conn = lease.get();
  if (conn == nullptr) {
    return false;
  }
  Statement statement(conn);
  const char* sql =
      "INSERT INTO game_records (room_id, player_x_id, player_o_id, winner, result, started_at) "
      "VALUES (?, ?, ?, ?, ?, FROM_UNIXTIME(?))";
  if (statement.handle == nullptr || mysql_stmt_prepare(statement.handle, sql, std::strlen(sql)) != 0) {
    logDb("save game prepare", statement.handle == nullptr ? "no statement" : mysql_stmt_error(statement.handle));
    return false;
  }
  int roomId = record.roomId;
  unsigned long long playerX = record.playerX;
  unsigned long long playerO = record.playerO;
  char winner = record.winner;
  bool winnerNull = record.winner == 'D';
  unsigned long winnerLen = 1;
  unsigned long resultLen = 0;
  long long started = record.startedAt;
  MYSQL_BIND params[6]{};
  params[0].buffer_type = MYSQL_TYPE_LONG;
  params[0].buffer = &roomId;
  params[1].buffer_type = MYSQL_TYPE_LONGLONG;
  params[1].buffer = &playerX;
  params[1].is_unsigned = 1;
  params[2].buffer_type = MYSQL_TYPE_LONGLONG;
  params[2].buffer = &playerO;
  params[2].is_unsigned = 1;
  params[3].buffer_type = MYSQL_TYPE_STRING;
  params[3].buffer = &winner;
  params[3].buffer_length = 1;
  params[3].length = &winnerLen;
  params[3].is_null = &winnerNull;
  bindString(params[4], record.result, resultLen);
  params[5].buffer_type = MYSQL_TYPE_LONGLONG;
  params[5].buffer = &started;
  if (mysql_stmt_bind_param(statement.handle, params) != 0 || mysql_stmt_execute(statement.handle) != 0) {
    if (mysql_stmt_errno(statement.handle) == kDuplicateKey) {
      return true;
    }
    logDb("save game", mysql_stmt_error(statement.handle));
    return false;
  }
  return true;
}

HistoryResult MysqlAccounts::history(std::uint64_t accountId, int limit) {
  HistoryResult outcome;
  if (limit < 1 || limit > 50) {
    outcome.code = proto::kInvalidCommand;
    outcome.detail = "history limit must be from 1 to 50";
    return outcome;
  }
  Pool::Lease lease = pool_->acquire();
  MYSQL* conn = lease.get();
  if (conn == nullptr) {
    outcome.code = proto::kDatabaseUnavailable;
    outcome.detail = "database unavailable";
    return outcome;
  }
  Statement statement(conn);
  const char* sql =
      "SELECT id, result, DATE_FORMAT(finished_at, '%Y-%m-%d %H:%i:%s') "
      "FROM game_records WHERE player_x_id = ? OR player_o_id = ? "
      "ORDER BY finished_at DESC, id DESC LIMIT ?";
  if (statement.handle == nullptr || mysql_stmt_prepare(statement.handle, sql, std::strlen(sql)) != 0) {
    logDb("history prepare", statement.handle == nullptr ? "no statement" : mysql_stmt_error(statement.handle));
    outcome.code = proto::kDatabaseUnavailable;
    outcome.detail = "database unavailable";
    return outcome;
  }
  unsigned long long idParam = accountId;
  int limitParam = limit;
  MYSQL_BIND params[3]{};
  params[0].buffer_type = MYSQL_TYPE_LONGLONG;
  params[0].buffer = &idParam;
  params[0].is_unsigned = 1;
  params[1].buffer_type = MYSQL_TYPE_LONGLONG;
  params[1].buffer = &idParam;
  params[1].is_unsigned = 1;
  params[2].buffer_type = MYSQL_TYPE_LONG;
  params[2].buffer = &limitParam;
  unsigned long long rowId = 0;
  char resultText[32];
  char finished[32];
  unsigned long resultLen = 0;
  unsigned long finishedLen = 0;
  MYSQL_BIND row[3]{};
  row[0].buffer_type = MYSQL_TYPE_LONGLONG;
  row[0].buffer = &rowId;
  row[0].is_unsigned = 1;
  row[1].buffer_type = MYSQL_TYPE_STRING;
  row[1].buffer = resultText;
  row[1].buffer_length = sizeof(resultText);
  row[1].length = &resultLen;
  row[2].buffer_type = MYSQL_TYPE_STRING;
  row[2].buffer = finished;
  row[2].buffer_length = sizeof(finished);
  row[2].length = &finishedLen;
  if (mysql_stmt_bind_param(statement.handle, params) != 0 || mysql_stmt_bind_result(statement.handle, row) != 0 ||
      mysql_stmt_execute(statement.handle) != 0 || mysql_stmt_store_result(statement.handle) != 0) {
    logDb("history", mysql_stmt_error(statement.handle));
    outcome.code = proto::kDatabaseUnavailable;
    outcome.detail = "database unavailable";
    return outcome;
  }
  while (true) {
    const int fetched = mysql_stmt_fetch(statement.handle);
    if (fetched == MYSQL_NO_DATA) {
      break;
    }
    if (fetched != 0 && fetched != MYSQL_DATA_TRUNCATED) {
      logDb("history fetch", mysql_stmt_error(statement.handle));
      outcome.code = proto::kDatabaseUnavailable;
      outcome.detail = "database unavailable";
      outcome.rows.clear();
      return outcome;
    }
    HistoryEntry entry;
    entry.id = rowId;
    entry.result.assign(resultText, resultLen < sizeof(resultText) ? resultLen : sizeof(resultText) - 1);
    entry.finishedAt.assign(finished, finishedLen < sizeof(finished) ? finishedLen : sizeof(finished) - 1);
    outcome.rows.push_back(std::move(entry));
  }
  outcome.ok = true;
  return outcome;
}

StatsResult MysqlAccounts::stats(std::uint64_t accountId) {
  StatsResult outcome;
  Pool::Lease lease = pool_->acquire();
  MYSQL* conn = lease.get();
  if (conn == nullptr) {
    outcome.code = proto::kDatabaseUnavailable;
    outcome.detail = "database unavailable";
    return outcome;
  }
  Statement statement(conn);
  const char* sql =
      "SELECT COUNT(*),"
      " COALESCE(SUM(CASE WHEN (player_x_id = ? AND result = 'X_WIN') OR (player_o_id = ? AND result = 'O_WIN') THEN 1 ELSE 0 END), 0),"
      " COALESCE(SUM(CASE WHEN result = 'DRAW' THEN 1 ELSE 0 END), 0) "
      "FROM game_records WHERE player_x_id = ? OR player_o_id = ?";
  if (statement.handle == nullptr || mysql_stmt_prepare(statement.handle, sql, std::strlen(sql)) != 0) {
    logDb("stats prepare", statement.handle == nullptr ? "no statement" : mysql_stmt_error(statement.handle));
    outcome.code = proto::kDatabaseUnavailable;
    outcome.detail = "database unavailable";
    return outcome;
  }
  unsigned long long idParam = accountId;
  MYSQL_BIND params[4]{};
  for (MYSQL_BIND& param : params) {
    param.buffer_type = MYSQL_TYPE_LONGLONG;
    param.buffer = &idParam;
    param.is_unsigned = 1;
  }
  long long games = 0;
  long long wins = 0;
  long long draws = 0;
  MYSQL_BIND row[3]{};
  row[0].buffer_type = MYSQL_TYPE_LONGLONG;
  row[0].buffer = &games;
  row[1].buffer_type = MYSQL_TYPE_LONGLONG;
  row[1].buffer = &wins;
  row[2].buffer_type = MYSQL_TYPE_LONGLONG;
  row[2].buffer = &draws;
  if (mysql_stmt_bind_param(statement.handle, params) != 0 || mysql_stmt_bind_result(statement.handle, row) != 0 ||
      mysql_stmt_execute(statement.handle) != 0 || mysql_stmt_store_result(statement.handle) != 0 ||
      mysql_stmt_fetch(statement.handle) != 0) {
    logDb("stats", mysql_stmt_error(statement.handle));
    outcome.code = proto::kDatabaseUnavailable;
    outcome.detail = "database unavailable";
    return outcome;
  }
  outcome.ok = true;
  outcome.games = static_cast<int>(games);
  outcome.wins = static_cast<int>(wins);
  outcome.draws = static_cast<int>(draws);
  outcome.losses = outcome.games - outcome.wins - outcome.draws;
  return outcome;
}

int MysqlAccounts::countGamesForRoom(int roomId) {
  Pool::Lease lease = pool_->acquire();
  MYSQL* conn = lease.get();
  if (conn == nullptr) {
    return -1;
  }
  Statement statement(conn);
  const char* sql = "SELECT COUNT(*) FROM game_records WHERE room_id = ?";
  if (statement.handle == nullptr || mysql_stmt_prepare(statement.handle, sql, std::strlen(sql)) != 0) {
    return -1;
  }
  MYSQL_BIND param{};
  param.buffer_type = MYSQL_TYPE_LONG;
  param.buffer = &roomId;
  long long count = 0;
  MYSQL_BIND result{};
  result.buffer_type = MYSQL_TYPE_LONGLONG;
  result.buffer = &count;
  if (mysql_stmt_bind_param(statement.handle, &param) != 0 || mysql_stmt_bind_result(statement.handle, &result) != 0 ||
      mysql_stmt_execute(statement.handle) != 0 || mysql_stmt_store_result(statement.handle) != 0 ||
      mysql_stmt_fetch(statement.handle) != 0) {
    return -1;
  }
  return static_cast<int>(count);
}

bool MysqlAccounts::inspectUser(const std::string& username, const std::string& password, bool& hashed,
                                bool& lastLoginSet) {
  hashed = false;
  lastLoginSet = false;
  Pool::Lease lease = pool_->acquire();
  MYSQL* conn = lease.get();
  if (conn == nullptr) {
    return false;
  }
  Statement statement(conn);
  const char* sql = "SELECT password_hash, last_login_at IS NOT NULL FROM users WHERE username = ?";
  if (statement.handle == nullptr || mysql_stmt_prepare(statement.handle, sql, std::strlen(sql)) != 0) {
    return false;
  }
  MYSQL_BIND param{};
  unsigned long nameLen = 0;
  bindString(param, username, nameLen);
  char hashBuffer[128];
  unsigned long hashLen = 0;
  long long loginFlag = 0;
  MYSQL_BIND result[2]{};
  result[0].buffer_type = MYSQL_TYPE_STRING;
  result[0].buffer = hashBuffer;
  result[0].buffer_length = sizeof(hashBuffer);
  result[0].length = &hashLen;
  result[1].buffer_type = MYSQL_TYPE_LONGLONG;
  result[1].buffer = &loginFlag;
  if (mysql_stmt_bind_param(statement.handle, &param) != 0 || mysql_stmt_bind_result(statement.handle, result) != 0 ||
      mysql_stmt_execute(statement.handle) != 0 || mysql_stmt_store_result(statement.handle) != 0) {
    return false;
  }
  if (mysql_stmt_fetch(statement.handle) != 0) {
    return false;
  }
  const std::string stored(hashBuffer, hashLen < sizeof(hashBuffer) ? hashLen : sizeof(hashBuffer) - 1);
  hashed = stored.compare(0, 4, "$2b$") == 0 && stored != password;
  lastLoginSet = loginFlag != 0;
  return true;
}

}  // namespace minigame
