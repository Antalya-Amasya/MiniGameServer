#pragma once

#include "minigame/TcpSocket.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef __linux__
namespace minigame {
class Reactor;
}
#endif

namespace minigame {

class RoomManager;
class Session;

// Phase 1 的网络模型：
// - 主线程用 select 等待监听套接字，或等待唤醒套接字（用于停止）。
// - 每个已接受的连接单独一个线程阻塞读。
//
// 这是以后换成 epoll + Reactor 时要替换的边界：
// - run() 里的 select/accept 循环
// - Session::run() 里的读循环
// Room / TicTacToeGame / 协议解析不依赖 select 还是 epoll。
//
// stop() 可以从别的线程调用。它只往唤醒套接字写一个字节，并对已连接套接字 shutdown。
// 真正 close 监听套接字和 join 线程都在 run() 里，避免在别的线程里关闭正在 accept/select 的 fd。
class Server {
 public:
  Server(std::uint16_t port, RoomManager& rooms);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  bool start();
  void run();
  void stop();
  void setIdleTimeout(std::chrono::milliseconds timeout);

  std::uint16_t port() const;

 private:
  struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished;
  };

  void reapFinishedWorkers();
  void stopSessions();
  void joinWorkers();
  void notifyStop();

  std::uint16_t port_ = 0;
  RoomManager& rooms_;
  TcpSocket listenSocket_;
#ifdef __linux__
  std::unique_ptr<Reactor> reactor_;
#else
  TcpSocket wakeupRead_;
  TcpSocket wakeupWrite_;
  std::atomic<bool> wakeupSent_{false};
#endif
  std::atomic<bool> stopping_{false};
  std::chrono::milliseconds idleTimeout_{std::chrono::seconds(60)};

  std::mutex sessionMu_;
  std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions_;

  std::mutex workerMu_;
  std::vector<Worker> workers_;
};

}  // namespace minigame
