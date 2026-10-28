#include "minigame/Logger.hpp"
#include "minigame/Protocol.hpp"
#include "minigame/RoomManager.hpp"
#include "minigame/Server.hpp"
#include "minigame/TcpSocket.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

int g_passed = 0;
int g_failed = 0;

class Probe {
 public:
  bool connectTo(std::uint16_t port) {
    if (!socket_.connectTo("127.0.0.1", port)) {
      return false;
    }
    socket_.setRecvTimeoutMs(3000);
    return true;
  }

  void sendLine(const std::string& line) {
    if (!socket_.sendAll(line + "\n")) {
      throw std::runtime_error("send failed: " + line);
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
        throw std::runtime_error("read timeout, pending=[" + pending_ + "]");
      }
      if (received <= 0) {
        throw std::runtime_error("connection closed, pending=[" + pending_ + "]");
      }
      pending_.append(temp, temp + received);
    }
  }

  void close() { socket_.close(); }

 private:
  minigame::TcpSocket socket_;
  std::string pending_;
};

struct ProbeGuard {
  Probe& probe;
  ~ProbeGuard() { probe.close(); }
};

void expectLine(Probe& probe, const std::string& expected) {
  const std::string actual = probe.readLine();
  std::cout << "  S> " << actual << "\n";
  if (actual != expected) {
    throw std::runtime_error("expected [" + expected + "] got [" + actual + "]");
  }
}

void expectBoard(Probe& probe, const std::string& row0, const std::string& row1, const std::string& row2) {
  expectLine(probe, "BOARD");
  expectLine(probe, row0);
  expectLine(probe, row1);
  expectLine(probe, row2);
}

int expectRoomCreated(Probe& probe) {
  const std::string line = probe.readLine();
  std::cout << "  S> " << line << "\n";
  const std::string prefix = "ROOM_CREATED ";
  if (line.rfind(prefix, 0) != 0) {
    throw std::runtime_error("expected ROOM_CREATED, got [" + line + "]");
  }
  return std::stoi(line.substr(prefix.size()));
}

void runScenario(const char* name, const std::function<void()>& function) {
  try {
    std::cout << "SCENARIO " << name << "\n";
    function();
    std::cout << "PASS " << name << "\n";
    ++g_passed;
  } catch (const std::exception& ex) {
    std::cout << "FAIL " << name << " " << ex.what() << "\n";
    ++g_failed;
  }
}

bool waitUntil(const std::function<bool()>& predicate) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

}  // namespace

int main() {
  minigame::SocketRuntime runtime;
  if (!runtime.ok()) {
    std::cerr << "socket runtime init failed\n";
    return 1;
  }

  minigame::RoomManager rooms;
  minigame::Server server(0, rooms);
  if (!server.start()) {
    std::cerr << "server failed to start\n";
    return 1;
  }

  minigame::RoomManager otherRooms;
  minigame::Server second(server.port(), otherRooms);
  if (second.start()) {
    std::cerr << "second server unexpectedly bound the same port\n";
    server.stop();
    return 1;
  }

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
  } running(server);

  const std::uint16_t port = server.port();
  std::cout << "integration server port " << port << "\n";

  runScenario("two_clients_play_a_game", [&]() {
    Probe alice;
    Probe bob;
    Probe carol;
    ProbeGuard aliceGuard{alice};
    ProbeGuard bobGuard{bob};
    ProbeGuard carolGuard{carol};
    if (!alice.connectTo(port) || !bob.connectTo(port) || !carol.connectTo(port)) {
      throw std::runtime_error("client connect failed");
    }

    alice.sendLine("CREATE_ROOM");
    expectLine(alice, "ERROR NOT_LOGGED_IN send LOGIN <name> first");
    alice.sendLine("LOGIN alice!");
    expectLine(alice, "ERROR BAD_NAME use 1-16 letters, digits, or underscore");
    alice.sendLine("LOGIN alice");
    expectLine(alice, "OK LOGIN alice");
    alice.sendLine("CREATE_ROOM");
    const int roomId = expectRoomCreated(alice);
    expectLine(alice, "WAITING_OPPONENT");
    alice.sendLine("MOVE 0 0");
    expectLine(alice, "ERROR GAME_NOT_STARTED waiting for the opponent");

    bob.sendLine("LOGIN bob");
    expectLine(bob, "OK LOGIN bob");
    bob.sendLine("JOIN_ROOM 99999");
    expectLine(bob, "ERROR ROOM_NOT_FOUND no such room");
    bob.sendLine("JOIN_ROOM " + std::to_string(roomId));
    expectLine(bob, "ROOM_JOINED " + std::to_string(roomId) + " O");
    expectLine(bob, "GAME_START O");
    expectBoard(bob, "...", "...", "...");
    expectLine(bob, "OPPONENT_TURN");

    expectLine(alice, "OPPONENT_JOINED bob");
    expectLine(alice, "GAME_START X");
    expectBoard(alice, "...", "...", "...");
    expectLine(alice, "YOUR_TURN");

    carol.sendLine("LOGIN carol");
    expectLine(carol, "OK LOGIN carol");
    carol.sendLine("JOIN_ROOM " + std::to_string(roomId));
    expectLine(carol, "ERROR ROOM_FULL room is not waiting for a player");
    carol.sendLine("QUIT");
    expectLine(carol, "OK QUIT");

    bob.sendLine("MOVE 1 1");
    expectLine(bob, "ERROR NOT_YOUR_TURN wait for the other player");
    alice.sendLine("MOVE 3 0");
    expectLine(alice, "ERROR OUT_OF_RANGE row and col must be 0..2");

    alice.sendLine("MOVE 0 0");
    expectBoard(alice, "X..", "...", "...");
    expectLine(alice, "OPPONENT_TURN");
    expectBoard(bob, "X..", "...", "...");
    expectLine(bob, "YOUR_TURN");

    bob.sendLine("MOVE 0 0");
    expectLine(bob, "ERROR CELL_OCCUPIED cell already used");
    bob.sendLine("MOVE 1 0");
    expectBoard(bob, "X..", "O..", "...");
    expectLine(bob, "OPPONENT_TURN");
    expectBoard(alice, "X..", "O..", "...");
    expectLine(alice, "YOUR_TURN");

    alice.sendLine("MOVE 0 1");
    expectBoard(alice, "XX.", "O..", "...");
    expectLine(alice, "OPPONENT_TURN");
    expectBoard(bob, "XX.", "O..", "...");
    expectLine(bob, "YOUR_TURN");

    bob.sendLine("MOVE 1 1");
    expectBoard(bob, "XX.", "OO.", "...");
    expectLine(bob, "OPPONENT_TURN");
    expectBoard(alice, "XX.", "OO.", "...");
    expectLine(alice, "YOUR_TURN");

    alice.sendLine("MOVE 0 2");
    expectBoard(alice, "XXX", "OO.", "...");
    expectLine(alice, "GAME_OVER X_WIN");
    expectBoard(bob, "XXX", "OO.", "...");
    expectLine(bob, "GAME_OVER X_WIN");

    alice.sendLine("MOVE 2 2");
    expectLine(alice, "ERROR GAME_OVER the game is already finished");
    alice.sendLine("CREATE_ROOM");
    expectLine(alice, "ERROR ALREADY_IN_ROOM leave the current room first");

    alice.sendLine("QUIT");
    expectLine(alice, "OK QUIT");
    expectLine(bob, "OPPONENT_LEFT");
    expectLine(bob, "GAME_OVER OPPONENT_LEFT");
    bob.sendLine("QUIT");
    expectLine(bob, "OK QUIT");
  });

  runScenario("abrupt_disconnect_keeps_server_alive", [&]() {
    Probe dave;
    Probe eve;
    ProbeGuard daveGuard{dave};
    ProbeGuard eveGuard{eve};
    if (!dave.connectTo(port) || !eve.connectTo(port)) {
      throw std::runtime_error("client connect failed");
    }
    dave.sendLine("LOGIN dave");
    expectLine(dave, "OK LOGIN dave");
    dave.sendLine("CREATE_ROOM");
    const int roomId = expectRoomCreated(dave);
    expectLine(dave, "WAITING_OPPONENT");

    eve.sendLine("LOGIN eve");
    expectLine(eve, "OK LOGIN eve");
    eve.sendLine("JOIN_ROOM " + std::to_string(roomId));
    expectLine(eve, "ROOM_JOINED " + std::to_string(roomId) + " O");
    expectLine(eve, "GAME_START O");
    expectBoard(eve, "...", "...", "...");
    expectLine(eve, "OPPONENT_TURN");
    expectLine(dave, "OPPONENT_JOINED eve");
    expectLine(dave, "GAME_START X");
    expectBoard(dave, "...", "...", "...");
    expectLine(dave, "YOUR_TURN");

    dave.close();
    expectLine(eve, "OPPONENT_LEFT");
    expectLine(eve, "GAME_OVER OPPONENT_LEFT");
    eve.sendLine("CREATE_ROOM");
    const int nextRoom = expectRoomCreated(eve);
    expectLine(eve, "WAITING_OPPONENT");
    if (nextRoom == roomId) {
      throw std::runtime_error("room id was reused immediately");
    }
    eve.sendLine("QUIT");
    expectLine(eve, "OK QUIT");
  });

  runScenario("garbage_does_not_kill_server", [&]() {
    Probe junk;
    ProbeGuard junkGuard{junk};
    if (!junk.connectTo(port)) {
      throw std::runtime_error("client connect failed");
    }
    junk.sendLine("");
    junk.sendLine("   ");
    junk.sendLine("HELLO");
    expectLine(junk, "ERROR INVALID_COMMAND unknown command");
    junk.sendLine("LOGIN frank");
    expectLine(junk, "OK LOGIN frank");
    junk.sendLine(std::string(1200, 'A'));
    expectLine(junk, "ERROR INVALID_COMMAND line too long");
    junk.close();

    Probe again;
    ProbeGuard againGuard{again};
    if (!again.connectTo(port)) {
      throw std::runtime_error("server did not accept a new client after garbage");
    }
    again.sendLine("LOGIN grace");
    expectLine(again, "OK LOGIN grace");
    again.sendLine("QUIT");
    expectLine(again, "OK QUIT");
  });

  runScenario("server_still_accepts_after_idle", [&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Probe again;
    ProbeGuard guard{again};
    if (!again.connectTo(port)) {
      throw std::runtime_error("server stopped accepting");
    }
    again.sendLine("LOGIN henry");
    expectLine(again, "OK LOGIN henry");
    again.sendLine("QUIT");
    expectLine(again, "OK QUIT");
  });

  runScenario("sessions_are_released", [&]() {
    if (!waitUntil([&rooms]() { return rooms.roomCount() == 0 && rooms.loggedInCount() == 0; })) {
      throw std::runtime_error("rooms=" + std::to_string(rooms.roomCount()) +
                               " loggedIn=" + std::to_string(rooms.loggedInCount()));
    }
  });

  std::cout << "INTEGRATION_SUMMARY tests=" << (g_passed + g_failed) << " passed=" << g_passed
            << " failed=" << g_failed << "\n";

  if (g_failed != 0) {
    return 1;
  }

  // 停掉服务器后，它必须还能从 run() 里返回。ServerRun 析构负责这件事。
  return 0;
}
