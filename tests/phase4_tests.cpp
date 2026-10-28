#include "minigame/Database.hpp"
#include "minigame/Logger.hpp"
#include "minigame/Protocol.hpp"
#include "minigame/RoomManager.hpp"
#include "minigame/Server.hpp"
#include "minigame/TcpSocket.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_passed = 0;
int g_failed = 0;

void require(bool condition, const std::string& detail) {
  if (!condition) {
    ++g_failed;
    std::cout << "FAIL " << detail << "\n";
    throw std::runtime_error(detail);
  }
  ++g_passed;
  std::cout << "PASS " << detail << "\n";
}

class Probe {
 public:
  bool connectTo(std::uint16_t port) {
    if (!socket_.connectTo("127.0.0.1", port)) {
      return false;
    }
    socket_.setRecvTimeoutMs(4000);
    return true;
  }

  void sendLine(const std::string& line) {
    if (!socket_.sendAll(line + "\n")) {
      throw std::runtime_error("send failed");
    }
  }

  std::string readLine() {
    while (true) {
      const std::size_t pos = pending_.find('\n');
      if (pos != std::string::npos) {
        std::string line = pending_.substr(0, pos);
        pending_.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        return line;
      }
      char temp[512];
      const int received = socket_.recvSome(temp, static_cast<int>(sizeof(temp)));
      if (received == -2) {
        throw std::runtime_error("read timeout");
      }
      if (received <= 0) {
        throw std::runtime_error("connection closed");
      }
      pending_.append(temp, temp + received);
    }
  }

  std::string readUntil(const std::string& prefix) {
    for (int attempt = 0; attempt < 40; ++attempt) {
      const std::string line = readLine();
      if (line.compare(0, prefix.size(), prefix) == 0) {
        return line;
      }
    }
    throw std::runtime_error("missing " + prefix);
  }

  void close() { socket_.close(); }

 private:
  minigame::TcpSocket socket_;
  std::string pending_;
};

struct ServerRun {
  minigame::Server& server;
  std::thread thread;
  explicit ServerRun(minigame::Server& value) : server(value), thread([&value]() { value.run(); }) {}
  ~ServerRun() {
    server.stop();
    if (thread.joinable()) {
      thread.join();
    }
  }
};

bool waitForRoomCount(minigame::MysqlAccounts& accounts, int roomId, int expected) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (accounts.countGamesForRoom(roomId) == expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

int roomIdFrom(const std::string& line) {
  const std::string prefix = "ROOM_CREATED ";
  return std::stoi(line.substr(prefix.size()));
}

void playUntilOver(Probe& first, Probe& second, const std::vector<std::pair<int, int>>& moves, bool firstStarts) {
  bool firstTurn = firstStarts;
  for (std::size_t index = 0; index < moves.size(); ++index) {
    Probe& actor = firstTurn ? first : second;
    actor.sendLine("MOVE " + std::to_string(moves[index].first) + " " + std::to_string(moves[index].second));
    const bool last = index + 1 == moves.size();
    actor.readUntil(last ? "GAME_OVER " : "OPPONENT_TURN");
    (firstTurn ? second : first).readUntil(last ? "GAME_OVER " : "YOUR_TURN");
    firstTurn = !firstTurn;
  }
}

}  // namespace

int main() {
  try {
    minigame::SocketRuntime runtime;
    require(runtime.ok(), "socket runtime");
    bool loadedEnv = false;
    const char* envPaths[] = {".env.local", "../.env.local", "../../.env.local"};
    for (const char* path : envPaths) {
      if (minigame::loadDotEnv(path)) {
        loadedEnv = true;
        break;
      }
    }
    require(loadedEnv, "env file");
    auto settings = minigame::settingsFromEnv("MINIGAME_TEST_DB_NAME");
    require(settings.has_value(), "test database settings");
    require(settings->database == "minigame_test", "tests use minigame_test");

    minigame::DbSettings deadSettings = *settings;
    deadSettings.port = 1;
    minigame::MysqlAccounts dead(deadSettings, 1);
    require(!dead.connected(), "dead database is not connected");
    const minigame::AuthResult deadRegister = dead.registerAccount("nobody", "secret1");
    require(!deadRegister.ok && deadRegister.code == minigame::proto::kDatabaseUnavailable, "dead database error");

    minigame::MysqlAccounts accounts(*settings);
    require(accounts.connected(), "test database connected");
    require(accounts.resetSchema(), "reset test schema");

    const minigame::AuthResult alice = accounts.registerAccount("alice", "secret1");
    const minigame::AuthResult bob = accounts.registerAccount("bob", "secret1");
    require(alice.ok && alice.accountId != 0, "register alice");
    require(bob.ok && bob.accountId != alice.accountId, "register bob");
    const minigame::AuthResult duplicate = accounts.registerAccount("alice", "secret1");
    require(!duplicate.ok && duplicate.code == minigame::proto::kUserExists, "duplicate username");
    const minigame::AuthResult bad = accounts.authenticate("alice", "wrongpass");
    require(!bad.ok && bad.code == minigame::proto::kInvalidCredentials, "wrong password");
    const minigame::AuthResult good = accounts.authenticate("alice", "secret1");
    require(good.ok && good.accountId == alice.accountId, "authenticate alice");
    bool hashed = false;
    bool lastLogin = false;
    require(accounts.inspectUser("alice", "secret1", hashed, lastLogin), "read alice row");
    require(hashed && lastLogin, "bcrypt hash and last login");

    minigame::FinishedRecord once;
    once.roomId = 4242;
    once.playerX = alice.accountId;
    once.playerO = bob.accountId;
    once.winner = 'D';
    once.result = "DRAW";
    once.startedAt = 1;
    require(accounts.saveGame(once), "first insert");
    require(accounts.saveGame(once), "duplicate insert is idempotent");
    require(accounts.countGamesForRoom(4242) == 1, "exactly one synthetic row");

    std::vector<std::thread> pingers;
    std::atomic<int> pings{0};
    for (int index = 0; index < 8; ++index) {
      pingers.emplace_back([&accounts, &pings]() {
        for (int attempt = 0; attempt < 10; ++attempt) {
          if (accounts.ping()) {
            pings.fetch_add(1);
          }
        }
      });
    }
    for (std::thread& thread : pingers) {
      thread.join();
    }
    require(pings.load() == 80, "pool handled 80 concurrent pings");

    {
    minigame::RoomManager rooms;
    rooms.setAccounts(&accounts);
    minigame::Server server(0, rooms);
    require(server.start(), "server start");
    ServerRun running(server);

    {
      Probe guest;
      require(guest.connectTo(server.port()), "guest connect");
      guest.sendLine("CREATE_ROOM");
      require(guest.readLine() == "ERROR NOT_LOGGED_IN send LOGIN <name> first", "guest cannot create");
      guest.sendLine("LOGIN alice");
      require(guest.readLine().find("ERROR INVALID_CREDENTIALS") == 0, "password required");
      guest.sendLine("LOGIN alice wrongpass");
      require(guest.readLine().find("ERROR INVALID_CREDENTIALS") == 0, "runtime wrong password");
      guest.close();
    }

    auto login = [](Probe& probe, const std::string& name) {
      probe.sendLine("REGISTER " + name + " secret1");
      const std::string registered = probe.readLine();
      if (registered.find("ERROR USER_EXISTS") != 0) {
        if (registered != "OK REGISTER " + name) {
          throw std::runtime_error("register failed");
        }
      }
      for (int attempt = 0; attempt < 20; ++attempt) {
        probe.sendLine("LOGIN " + name + " secret1");
        const std::string line = probe.readLine();
        if (line == "OK LOGIN " + name) {
          return;
        }
        if (line.find("ERROR ALREADY_ONLINE") != 0) {
          throw std::runtime_error("login failed");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      throw std::runtime_error("login stayed online");
    };

    const std::vector<std::pair<int, int>> xWin = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {0, 2}};
    const std::vector<std::pair<int, int>> oWin = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {2, 2}, {1, 2}};
    const std::vector<std::pair<int, int>> draw = {{0, 0}, {0, 1}, {0, 2}, {1, 1}, {1, 0},
                                                    {2, 0}, {1, 2}, {2, 2}, {2, 1}};

    int xRoom = 0;
    int oRoom = 0;
    int drawRoom = 0;
    {
      Probe left;
      Probe right;
      Probe extra;
      require(left.connectTo(server.port()) && right.connectTo(server.port()), "players connect");
      login(left, "alice");
      login(right, "bob");
      require(extra.connectTo(server.port()), "second alice connect");
      extra.sendLine("LOGIN alice secret1");
      require(extra.readLine().find("ERROR ALREADY_ONLINE") == 0, "duplicate online login");
      extra.close();

      left.sendLine("CREATE_ROOM");
      xRoom = roomIdFrom(left.readUntil("ROOM_CREATED "));
      left.readUntil("WAITING_OPPONENT");
      right.sendLine("JOIN_ROOM " + std::to_string(xRoom));
      right.readUntil("OPPONENT_TURN");
      left.readUntil("YOUR_TURN");
      playUntilOver(left, right, xWin, true);
      require(waitForRoomCount(accounts, xRoom, 1), "x win stored once");

      left.sendLine("QUIT");
      right.sendLine("QUIT");
      left.close();
      right.close();
    }
    {
      Probe left;
      Probe right;
      require(left.connectTo(server.port()) && right.connectTo(server.port()), "o-win connect");
      login(left, "alice");
      login(right, "bob");
      left.sendLine("CREATE_ROOM");
      oRoom = roomIdFrom(left.readUntil("ROOM_CREATED "));
      left.readUntil("WAITING_OPPONENT");
      right.sendLine("JOIN_ROOM " + std::to_string(oRoom));
      right.readUntil("OPPONENT_TURN");
      left.readUntil("YOUR_TURN");
      playUntilOver(left, right, oWin, true);
      require(waitForRoomCount(accounts, oRoom, 1), "o win stored once");
      left.close();
      right.close();
    }
    {
      Probe left;
      Probe right;
      require(left.connectTo(server.port()) && right.connectTo(server.port()), "draw connect");
      login(left, "alice");
      login(right, "bob");
      left.sendLine("CREATE_ROOM");
      drawRoom = roomIdFrom(left.readUntil("ROOM_CREATED "));
      left.readUntil("WAITING_OPPONENT");
      right.sendLine("JOIN_ROOM " + std::to_string(drawRoom));
      right.readUntil("OPPONENT_TURN");
      left.readUntil("YOUR_TURN");
      playUntilOver(left, right, draw, true);
      require(waitForRoomCount(accounts, drawRoom, 1), "draw stored once");
      left.sendLine("HISTORY 10");
      require(left.readLine() == "HISTORY 4", "alice history includes three games and the synthetic draw");
      int rows = 0;
      while (true) {
        const std::string line = left.readLine();
        if (line == "END_HISTORY") {
          break;
        }
        ++rows;
      }
      require(rows == 4, "history row count");
      left.sendLine("STATS");
      const std::string stats = left.readLine();
      require(stats == "STATS games=4 wins=1 losses=1 draws=2", stats);
      right.sendLine("STATS");
      const std::string bobStats = right.readLine();
      require(bobStats == "STATS games=4 wins=1 losses=1 draws=2", bobStats);
      left.close();
      right.close();
    }

    for (int index = 0; index < 20; ++index) {
      Probe probe;
      require(probe.connectTo(server.port()), "bulk connect");
      const std::string name = "u" + std::to_string(index);
      probe.sendLine("REGISTER " + name + " secret1");
      require(probe.readLine() == "OK REGISTER " + name, "bulk register");
      probe.sendLine("LOGIN " + name + " secret1");
      require(probe.readLine() == "OK LOGIN " + name, "bulk login");
      probe.sendLine("CREATE_ROOM");
      probe.readUntil("ROOM_CREATED ");
      probe.sendLine("QUIT");
      probe.close();
    }
    }
    require(accounts.ping(), "database still answers after shutdown");
    require(accounts.severIdleConnections(), "sever idle database sockets");
    require(accounts.ping(), "pool replaced severed connections");
    const minigame::AuthResult recovered = accounts.authenticate("alice", "secret1");
    require(recovered.ok, "login works after reconnect");
    std::cout << "phase4 checks passed=" << g_passed << " failed=" << g_failed << "\n";
    return g_failed == 0 ? 0 : 1;
  } catch (const std::exception& ex) {
    std::cout << "phase4 exception: " << ex.what() << "\n";
    std::cout << "phase4 checks passed=" << g_passed << " failed=" << g_failed << "\n";
    return 1;
  }
}
