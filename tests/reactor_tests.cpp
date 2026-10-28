#include "minigame/Logger.hpp"
#include "minigame/RoomManager.hpp"
#include "minigame/Server.hpp"
#include "minigame/TcpSocket.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

  void sendRaw(const std::string& bytes) {
    if (!socket_.sendAll(bytes)) {
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
        throw std::runtime_error("read timeout pending=[" + pending_ + "]");
      }
      if (received <= 0) {
        throw std::runtime_error("closed pending=[" + pending_ + "]");
      }
      pending_.append(temp, temp + received);
    }
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
    if (joined_) {
      return;
    }
    server.stop();
    if (thread.joinable()) {
      thread.join();
    }
  }

  void stopAndJoin() {
    const auto started = std::chrono::steady_clock::now();
    server.stop();
    if (thread.joinable()) {
      thread.join();
    }
    joined_ = true;
    if (std::chrono::steady_clock::now() - started > std::chrono::seconds(2)) {
      throw std::runtime_error("reactor stop took too long");
    }
  }

  bool joined_ = false;
};

void run(const char* name, const std::function<void(std::uint16_t)>& function, std::uint16_t port) {
  try {
    std::cout << "SCENARIO " << name << "\n";
    function(port);
    std::cout << "PASS " << name << "\n";
    ++g_passed;
  } catch (const std::exception& ex) {
    std::cout << "FAIL " << name << " " << ex.what() << "\n";
    ++g_failed;
  }
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
    std::cerr << "server start failed\n";
    return 1;
  }
  ServerRun running(server);
  const std::uint16_t port = server.port();

  run("sticky_two_commands", [](std::uint16_t port) {
    Probe probe;
    if (!probe.connectTo(port)) {
      throw std::runtime_error("connect");
    }
    probe.sendRaw("LOGIN sticky\nCREATE_ROOM\n");
    if (probe.readLine() != "OK LOGIN sticky") {
      throw std::runtime_error("login");
    }
    const std::string created = probe.readLine();
    if (created.rfind("ROOM_CREATED ", 0) != 0) {
      throw std::runtime_error(created);
    }
    if (probe.readLine() != "WAITING_OPPONENT") {
      throw std::runtime_error("waiting");
    }
    probe.close();
  }, port);

  run("half_packet_login", [](std::uint16_t port) {
    Probe probe;
    if (!probe.connectTo(port)) {
      throw std::runtime_error("connect");
    }
    probe.sendRaw("LOG");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    probe.sendRaw("IN half\n");
    if (probe.readLine() != "OK LOGIN half") {
      throw std::runtime_error("login");
    }
    probe.close();
  }, port);

  run("frames_split_across_writes", [](std::uint16_t port) {
    Probe probe;
    if (!probe.connectTo(port)) {
      throw std::runtime_error("connect");
    }
    probe.sendRaw("LOGIN split\nCRE");
    if (probe.readLine() != "OK LOGIN split") {
      throw std::runtime_error("login");
    }
    probe.sendRaw("ATE_ROOM\n");
    if (probe.readLine().rfind("ROOM_CREATED ", 0) != 0) {
      throw std::runtime_error("room");
    }
    if (probe.readLine() != "WAITING_OPPONENT") {
      throw std::runtime_error("waiting");
    }
    probe.close();
  }, port);

  run("malformed_does_not_kill_server", [](std::uint16_t port) {
    Probe bad;
    if (!bad.connectTo(port)) {
      throw std::runtime_error("connect");
    }
    bad.sendRaw("HELLO\n");
    if (bad.readLine() != "ERROR INVALID_COMMAND unknown command") {
      throw std::runtime_error("error text");
    }
    bad.close();
    Probe good;
    if (!good.connectTo(port)) {
      throw std::runtime_error("server died");
    }
    good.sendRaw("LOGIN good\n");
    if (good.readLine() != "OK LOGIN good") {
      throw std::runtime_error("login");
    }
    good.close();
  }, port);

  run("one_disconnect_leaves_the_other", [](std::uint16_t port) {
    Probe left;
    Probe right;
    if (!left.connectTo(port) || !right.connectTo(port)) {
      throw std::runtime_error("connect");
    }
    left.sendRaw("LOGIN left\n");
    right.sendRaw("LOGIN right\n");
    if (left.readLine() != "OK LOGIN left" || right.readLine() != "OK LOGIN right") {
      throw std::runtime_error("login");
    }
    left.close();
    left.close();
    right.sendRaw("CREATE_ROOM\n");
    if (right.readLine().rfind("ROOM_CREATED ", 0) != 0) {
      throw std::runtime_error("right could not create");
    }
    right.close();
  }, port);

  run("output_contains_full_login_line", [](std::uint16_t port) {
    Probe probe;
    if (!probe.connectTo(port)) {
      throw std::runtime_error("connect");
    }
    probe.sendRaw("LOGIN full\n");
    if (probe.readLine() != "OK LOGIN full") {
      throw std::runtime_error("truncated response");
    }
    probe.close();
  }, port);

  run("many_clients_online", [](std::uint16_t port) {
    std::vector<Probe> probes(8);
    for (int index = 0; index < 8; ++index) {
      if (!probes[static_cast<std::size_t>(index)].connectTo(port)) {
        throw std::runtime_error("connect");
      }
      probes[static_cast<std::size_t>(index)].sendRaw("LOGIN user" + std::to_string(index) + "\n");
    }
    for (int index = 0; index < 8; ++index) {
      const std::string line = probes[static_cast<std::size_t>(index)].readLine();
      if (line != "OK LOGIN user" + std::to_string(index)) {
        throw std::runtime_error(line);
      }
    }
    for (Probe& probe : probes) {
      probe.close();
    }
  }, port);

  run("oversized_line_then_new_client", [](std::uint16_t port) {
    Probe probe;
    if (!probe.connectTo(port)) {
      throw std::runtime_error("connect");
    }
    probe.sendRaw(std::string(1200, 'A') + "\n");
    if (probe.readLine() != "ERROR INVALID_COMMAND line too long") {
      throw std::runtime_error("expected too long");
    }
    probe.close();
    Probe next;
    if (!next.connectTo(port)) {
      throw std::runtime_error("server died");
    }
    next.sendRaw("LOGIN afterlong\n");
    if (next.readLine() != "OK LOGIN afterlong") {
      throw std::runtime_error("login");
    }
    next.close();
  }, port);

  try {
    running.stopAndJoin();
    std::cout << "PASS reactor_stop\n";
    ++g_passed;
  } catch (const std::exception& ex) {
    std::cout << "FAIL reactor_stop " << ex.what() << "\n";
    ++g_failed;
  }
  std::cout << "REACTOR_SUMMARY tests=" << (g_passed + g_failed) << " passed=" << g_passed
            << " failed=" << g_failed << "\n";
  return g_failed == 0 ? 0 : 1;
}
