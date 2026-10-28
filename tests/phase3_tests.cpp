#include "minigame/OutputBuffer.hpp"
#include "minigame/ResponseQueue.hpp"
#include "minigame/RoomManager.hpp"
#include "minigame/Server.hpp"
#include "minigame/SessionLane.hpp"
#include "minigame/TcpSocket.hpp"
#include "minigame/ThreadPool.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_passed = 0;
int g_failed = 0;

void run(const char* name, void (*function)()) {
  try {
    function();
    std::cout << "PASS " << name << "\n";
    ++g_passed;
  } catch (const std::exception& ex) {
    std::cout << "FAIL " << name << " " << ex.what() << "\n";
    ++g_failed;
  }
}

void require(bool condition, const std::string& text) {
  if (!condition) {
    throw std::runtime_error(text);
  }
}

void testPoolRunsTasks() {
  std::atomic<int> count{0};
  {
    minigame::ThreadPool pool(2, 32);
    require(pool.submit([&count]() { count.fetch_add(1); }), "submit");
    require(pool.submit([&count]() { count.fetch_add(1); }), "submit2");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  require(count.load() == 2, "count");
}

void testPoolMultipleWorkers() {
  constexpr int kTasks = 40;
  std::atomic<int> count{0};
  minigame::ThreadPool pool(4, 64);
  for (int index = 0; index < kTasks; ++index) {
    require(pool.submit([&count]() { count.fetch_add(1); }), "submit");
  }
  pool.stop();
  require(count.load() == kTasks, "all tasks");
}

void testPoolDrainsQueuedTasks() {
  std::atomic<int> count{0};
  minigame::ThreadPool pool(1, 16);
  require(pool.submit([&count]() { std::this_thread::sleep_for(std::chrono::milliseconds(30)); count.fetch_add(1); }),
          "first");
  require(pool.submit([&count]() { count.fetch_add(1); }), "second");
  pool.stop();
  require(count.load() == 2, "drained");
  require(!pool.submit([]() {}), "rejected after stop");
}

void testPoolExceptionDoesNotKillWorker() {
  std::atomic<bool> ran{false};
  minigame::ThreadPool pool(1, 8);
  require(pool.submit([]() { throw std::runtime_error("boom"); }), "throw");
  require(pool.submit([&ran]() { ran.store(true); }), "after");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  pool.stop();
  require(ran.load(), "worker survived");
}

void testQueueLimit() {
  std::mutex gate;
  std::condition_variable condition;
  bool release = false;
  minigame::ThreadPool pool(1, 2);
  require(pool.submit([&]() {
            std::unique_lock<std::mutex> lock(gate);
            condition.wait(lock, [&release]() { return release; });
          }),
          "blocker");
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  require(pool.submit([]() {}), "q1");
  require(pool.submit([]() {}), "q2");
  require(!pool.submit([]() {}), "over limit");
  {
    std::lock_guard<std::mutex> lock(gate);
    release = true;
  }
  condition.notify_all();
  pool.stop();
}

void testSessionOrder() {
  minigame::ThreadPool pool(4, 32);
  std::shared_ptr<minigame::SessionLane> lane = minigame::SessionLane::create(pool, 16);
  std::mutex mutex;
  std::condition_variable condition;
  bool release = false;
  std::vector<int> order;
  require(lane->post([&]() {
            std::unique_lock<std::mutex> lock(mutex);
            order.push_back(1);
            condition.wait(lock, [&release]() { return release; });
          }),
          "first");
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  require(lane->post([&]() {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(2);
          }),
          "second");
  require(lane->post([&]() {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(3);
          }),
          "third");
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  condition.notify_all();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  pool.stop();
  require(order.size() == 3 && order[0] == 1 && order[1] == 2 && order[2] == 3, "order");
}

void testStaleConnectionIdIsDifferent() {
  minigame::ResponseQueue queue(8);
  minigame::PendingResponse stale;
  stale.connectionId = 1;
  stale.bytes = "OLD\n";
  minigame::PendingResponse current;
  current.connectionId = 2;
  current.bytes = "NEW\n";
  require(queue.push(stale), "push old");
  require(queue.push(std::move(current)), "push new");
  const std::vector<minigame::PendingResponse> batch = queue.drain();
  const std::uint64_t liveId = 2;
  std::string delivered;
  for (const minigame::PendingResponse& response : batch) {
    if (response.connectionId == liveId) {
      delivered += response.bytes;
    }
  }
  require(delivered == "NEW\n", delivered);
}

void testOutputLimit() {
  minigame::OutputBuffer buffer;
  require(buffer.tryAppend("abc", 3), "fits");
  require(!buffer.tryAppend("d", 3), "over");
  require(buffer.size() == 3, "size");
}

class Probe {
 public:
  bool connectTo(std::uint16_t port) {
    if (!socket_.connectTo("127.0.0.1", port)) {
      return false;
    }
    socket_.setRecvTimeoutMs(2000);
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
        return line;
      }
      char temp[512];
      const int received = socket_.recvSome(temp, static_cast<int>(sizeof(temp)));
      if (received == -2) {
        throw std::runtime_error("timeout");
      }
      if (received <= 0) {
        throw std::runtime_error("closed");
      }
      pending_.append(temp, temp + received);
    }
  }

  bool waitClosed() {
    socket_.setRecvTimeoutMs(2000);
    char temp[64];
    const int received = socket_.recvSome(temp, static_cast<int>(sizeof(temp)));
    return received <= 0;
  }

  void close() { socket_.close(); }

 private:
  minigame::TcpSocket socket_;
  std::string pending_;
};

struct ServerThread {
  minigame::Server& server;
  std::thread thread;
  bool stopped_ = false;
  explicit ServerThread(minigame::Server& value) : server(value), thread([&value]() { value.run(); }) {}
  ~ServerThread() { stopAndJoin(); }
  void stopAndJoin() {
    if (stopped_) {
      return;
    }
    stopped_ = true;
    server.stop();
    if (thread.joinable()) {
      thread.join();
    }
  }
};

void testIdleTimeoutAndActivity() {
  minigame::SocketRuntime runtime;
  require(runtime.ok(), "runtime");
  minigame::RoomManager rooms;
  minigame::Server server(0, rooms);
  server.setIdleTimeout(std::chrono::milliseconds(400));
  require(server.start(), "start");
  ServerThread loop(server);
  const std::uint16_t port = server.port();

  Probe idle;
  Probe active;
  require(idle.connectTo(port) && active.connectTo(port), "connect");
  active.sendRaw("PING\n");
  require(active.readLine() == "PONG", "pong");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
  while (std::chrono::steady_clock::now() < deadline) {
    active.sendRaw("PING\n");
    require(active.readLine() == "PONG", "pong loop");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }
  require(idle.waitClosed(), "idle closed");
  active.sendRaw("PING\n");
  require(active.readLine() == "PONG", "active survived");
  active.close();
  idle.close();
}

void testConcurrentRoomsAndDisconnect() {
  minigame::SocketRuntime runtime;
  require(runtime.ok(), "runtime");
  minigame::RoomManager rooms;
  minigame::Server server(0, rooms);
  require(server.start(), "start");
  ServerThread loop(server);
  const std::uint16_t port = server.port();

  Probe alice;
  Probe bob;
  Probe cara;
  Probe dan;
  require(alice.connectTo(port) && bob.connectTo(port) && cara.connectTo(port) && dan.connectTo(port), "connect");
  alice.sendRaw("LOGIN alice\nCREATE_ROOM\n");
  require(alice.readLine() == "OK LOGIN alice", "alice login");
  const std::string created = alice.readLine();
  require(created.rfind("ROOM_CREATED ", 0) == 0, created);
  require(alice.readLine() == "WAITING_OPPONENT", "wait");
  bob.sendRaw("LOGIN bob\n");
  require(bob.readLine() == "OK LOGIN bob", "bob");
  const std::string room = created.substr(std::string("ROOM_CREATED ").size());
  bob.sendRaw("JOIN_ROOM " + room + "\n");
  require(bob.readLine().rfind("ROOM_JOINED ", 0) == 0, "join");

  cara.sendRaw("LOGIN cara\nCREATE_ROOM\n");
  require(cara.readLine() == "OK LOGIN cara", "cara");
  require(cara.readLine().rfind("ROOM_CREATED ", 0) == 0, "room2");
  dan.sendRaw("LOGIN dan\nMOVE 0 0\n");
  require(dan.readLine() == "OK LOGIN dan", "dan");
  require(dan.readLine().rfind("ERROR ", 0) == 0, "dan not in room");

  bool aliceReady = false;
  for (int index = 0; index < 12 && !aliceReady; ++index) {
    if (alice.readLine() == "YOUR_TURN") {
      aliceReady = true;
    }
  }
  require(aliceReady, "alice turn");
  alice.sendRaw("MOVE 0 0\n");
  bob.sendRaw("MOVE 0 0\n");
  bool aliceMoved = false;
  bool bobAnswered = false;
  for (int index = 0; index < 16 && (!aliceMoved || !bobAnswered); ++index) {
    if (!aliceMoved && alice.readLine() == "OPPONENT_TURN") {
      aliceMoved = true;
    }
    if (!bobAnswered) {
      const std::string line = bob.readLine();
      if (line.rfind("ERROR ", 0) == 0 || line == "YOUR_TURN" || line == "OPPONENT_TURN") {
        bobAnswered = true;
      }
    }
  }
  require(aliceMoved && bobAnswered, "concurrent moves answered");
  alice.close();
  bool sawLeft = false;
  for (int index = 0; index < 20 && !sawLeft; ++index) {
    if (bob.readLine() == "OPPONENT_LEFT") {
      sawLeft = true;
    }
  }
  require(sawLeft, "opponent left");
  bob.close();
  cara.close();
  dan.close();

  std::string reused;
  for (int attempt = 0; attempt < 20 && reused != "OK LOGIN alice"; ++attempt) {
    Probe again;
    require(again.connectTo(port), "reconnect");
    again.sendRaw("LOGIN alice\n");
    reused = again.readLine();
    again.close();
    if (reused != "OK LOGIN alice") {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  require(reused == "OK LOGIN alice", reused);
}

void testStopWithConnections() {
  minigame::SocketRuntime runtime;
  require(runtime.ok(), "runtime");
  minigame::RoomManager rooms;
  minigame::Server server(0, rooms);
  require(server.start(), "start");
  ServerThread loop(server);
  std::vector<Probe> probes(6);
  for (Probe& probe : probes) {
    require(probe.connectTo(server.port()), "connect");
    probe.sendRaw("PING\n");
    require(probe.readLine() == "PONG", "pong");
  }
  const auto started = std::chrono::steady_clock::now();
  loop.stopAndJoin();
  require(std::chrono::steady_clock::now() - started < std::chrono::seconds(2), "slow stop");
  for (Probe& probe : probes) {
    probe.close();
  }
}

}  // namespace

int main() {
  run("pool_runs_tasks", testPoolRunsTasks);
  run("pool_multiple_workers", testPoolMultipleWorkers);
  run("pool_drains_before_stop", testPoolDrainsQueuedTasks);
  run("pool_exception_does_not_kill_worker", testPoolExceptionDoesNotKillWorker);
  run("pool_queue_limit", testQueueLimit);
  run("session_command_order", testSessionOrder);
  run("stale_connection_id_discarded", testStaleConnectionIdIsDifferent);
  run("output_buffer_limit", testOutputLimit);
  run("idle_timeout_and_activity_refresh", testIdleTimeoutAndActivity);
  run("concurrent_rooms_disconnect_and_reuse", testConcurrentRoomsAndDisconnect);
  run("stop_with_active_connections", testStopWithConnections);
  std::cout << "PHASE3_SUMMARY tests=" << (g_passed + g_failed) << " passed=" << g_passed
            << " failed=" << g_failed << "\n";
  return g_failed == 0 ? 0 : 1;
}
