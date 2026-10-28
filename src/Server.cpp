#include "minigame/Server.hpp"

#include "minigame/Logger.hpp"
#include "minigame/Session.hpp"

#ifdef __linux__
#include "minigame/Reactor.hpp"
#endif

#include <chrono>
#include <utility>

namespace minigame {

Server::Server(std::uint16_t port, RoomManager& rooms) : port_(port), rooms_(rooms) {}

Server::~Server() {
  // 调用方必须先让 run() 返回。这里只负责再发一次停止信号。
  stop();
#ifndef __linux__
  listenSocket_.close();
  wakeupRead_.close();
  wakeupWrite_.close();
#endif
}

bool Server::start() {
  if (!listenSocket_.listenOn(port_)) {
    return false;
  }
  port_ = listenSocket_.boundPort();
  if (port_ == 0) {
    log::error("bound port is 0");
    listenSocket_.close();
    return false;
  }
#ifdef __linux__
  if (!listenSocket_.setNonBlocking()) {
    log::error("failed to set listen socket non-blocking");
    listenSocket_.close();
    return false;
  }
  reactor_ = std::make_unique<Reactor>(std::move(listenSocket_), rooms_, idleTimeout_);
  if (!reactor_->arm()) {
    log::error("failed to arm reactor");
    return false;
  }
#else
  if (!createConnectedPair(wakeupRead_, wakeupWrite_)) {
    listenSocket_.close();
    return false;
  }
#endif
  log::info("listening on port " + std::to_string(port_));
  return true;
}

void Server::run() {
#ifdef __linux__
  if (!reactor_) {
    log::error("server run before start");
    return;
  }
  reactor_->loop();
#else
  if (!listenSocket_.valid()) {
    log::error("server run before start");
    return;
  }

  // 无论从哪条路径离开，都在这个线程里关闭监听套接字并回收连接线程。
  struct Cleanup {
    Server& server;
    ~Cleanup() {
      server.listenSocket_.close();
      server.stopSessions();
      server.joinWorkers();
      log::info("accept loop stopped");
    }
  } cleanup{*this};

  if (stopping_.load()) {
    return;
  }

  log::info("accept loop started");
  while (!stopping_.load()) {
    reapFinishedWorkers();
    const IoWait ready = waitForReadable(listenSocket_, wakeupRead_);
    // 唤醒套接字只负责打断 select。是否停止以 stopping_ 为准，
    // 避免把一次无关的可读事件当成关机。
    if (stopping_.load()) {
      break;
    }
    if (ready == IoWait::Woken) {
      char discarded = 0;
      wakeupRead_.recvSome(&discarded, 1);
      continue;
    }
    if (ready != IoWait::Ready) {
      log::warn("select failed");
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    TcpSocket client = listenSocket_.acceptClient();
    if (!client.valid()) {
      if (stopping_.load() || !listenSocket_.valid()) {
        break;
      }
      continue;
    }

    std::shared_ptr<Session> session = std::make_shared<Session>(std::move(client), rooms_);
    {
      std::lock_guard<std::mutex> lock(sessionMu_);
      sessions_.emplace(session->id(), session);
    }
    log::info("accepted session id=" + std::to_string(session->id()));

    std::shared_ptr<std::atomic<bool>> finished = std::make_shared<std::atomic<bool>>(false);
    try {
      std::thread worker([this, session, finished]() {
        session->run();
        {
          std::lock_guard<std::mutex> lock(sessionMu_);
          sessions_.erase(session->id());
        }
        finished->store(true);
      });
      std::lock_guard<std::mutex> lock(workerMu_);
      workers_.push_back(Worker{std::move(worker), std::move(finished)});
    } catch (const std::exception& ex) {
      log::error(std::string("failed to start session thread: ") + ex.what());
      session->requestStop();
      std::lock_guard<std::mutex> lock(sessionMu_);
      sessions_.erase(session->id());
    }
  }
#endif
}

void Server::setIdleTimeout(std::chrono::milliseconds timeout) {
  if (timeout.count() > 0) {
    idleTimeout_ = timeout;
  }
}

void Server::stop() {
  stopping_.store(true);
#ifdef __linux__
  if (reactor_) {
    reactor_->requestStop();
  }
#else
  notifyStop();
  stopSessions();
#endif
}

#ifndef __linux__
void Server::notifyStop() {
  bool expected = false;
  if (!wakeupSent_.compare_exchange_strong(expected, true)) {
    return;
  }
  // start() 失败时没有唤醒套接字。析构里的 stop() 不能把它当成运行中的服务器。
  if (!wakeupWrite_.valid()) {
    return;
  }
  if (!wakeupWrite_.sendAll("x")) {
    log::warn("failed to notify accept loop: " + wakeupWrite_.lastError());
  }
}
#endif

std::uint16_t Server::port() const {
  return port_;
}

void Server::reapFinishedWorkers() {
  std::lock_guard<std::mutex> lock(workerMu_);
  std::vector<Worker> stillRunning;
  stillRunning.reserve(workers_.size());
  for (Worker& worker : workers_) {
    if (worker.finished && worker.finished->load()) {
      if (worker.thread.joinable()) {
        worker.thread.join();
      }
    } else {
      stillRunning.push_back(std::move(worker));
    }
  }
  workers_ = std::move(stillRunning);
}

void Server::stopSessions() {
  std::vector<std::shared_ptr<Session>> snapshot;
  {
    std::lock_guard<std::mutex> lock(sessionMu_);
    snapshot.reserve(sessions_.size());
    for (const auto& entry : sessions_) {
      snapshot.push_back(entry.second);
    }
  }
  for (const std::shared_ptr<Session>& session : snapshot) {
    session->requestStop();
  }
}

void Server::joinWorkers() {
  std::lock_guard<std::mutex> lock(workerMu_);
  for (Worker& worker : workers_) {
    if (worker.thread.joinable()) {
      worker.thread.join();
    }
  }
  workers_.clear();
}

}  // namespace minigame
