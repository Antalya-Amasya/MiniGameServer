#pragma once

#include "minigame/Epoll.hpp"
#include "minigame/ResponseQueue.hpp"
#include "minigame/TcpSocket.hpp"
#include "minigame/ThreadPool.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>

namespace minigame {

class Connection;
class RoomManager;

// 单线程 Reactor。socket 读写只发生在这个线程。
// 业务任务进 ThreadPool，结果经 ResponseQueue 回到这里再写 output buffer。
class Reactor {
 public:
  Reactor(TcpSocket listenSocket, RoomManager& rooms, std::chrono::milliseconds idleTimeout);
  ~Reactor();

  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;

  bool arm();
  void loop();
  void wake();
  void requestStop();
  void updateInterest(Connection& connection);
  void postResponse(std::uint64_t connectionId, std::string bytes, bool closeAfterWrite);

  std::size_t workerCount() const;

 private:
  void acceptReady();
  void applyResponses();
  void checkIdle();
  void destroyConnection(std::uint64_t connectionId);
  void collectClosed();
  void destroyAll();
  void shutdownPool();
  bool armTimer();

  Epoll epoll_;
  TcpSocket listenSocket_;
  RoomManager& rooms_;
  ThreadPool pool_;
  ResponseQueue responses_;
  std::chrono::milliseconds idleTimeout_;
  int eventFd_ = -1;
  int timerFd_ = -1;
  bool stopping_ = false;
  bool poolStopped_ = false;
  std::unordered_map<std::uint64_t, std::shared_ptr<Connection>> connections_;

  static char listenToken_;
  static char wakeToken_;
  static char timerToken_;
};

}  // namespace minigame
