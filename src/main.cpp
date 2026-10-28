#include "minigame/Logger.hpp"
#include "minigame/RoomManager.hpp"
#include "minigame/Server.hpp"
#include "minigame/TcpSocket.hpp"

#ifdef MINIGAME_HAS_MYSQL
#include "minigame/Database.hpp"
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// 信号处理函数只写这个标志。关闭套接字放到普通线程里做，
// 因为 stop() 要加锁，不能放进异步信号上下文。
volatile std::sig_atomic_t g_stopRequested = 0;

void onSignal(int) {
  g_stopRequested = 1;
}

#ifdef _WIN32
BOOL WINAPI onConsoleCtrl(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
    g_stopRequested = 1;
    return TRUE;
  }
  return FALSE;
}
#endif

void printUsage() {
  std::cerr << "Usage: minigame_server [port]\n"
            << "       minigame_server --port <port> [--idle-ms <milliseconds>]\n"
            << "Default port is 9000. Port 0 asks the OS for a free port.\n"
            << "Default idle timeout is 60000 ms.\n";
}

bool parsePort(const std::string& text, std::uint16_t& port) {
  if (text.empty() || text.size() > 5) {
    return false;
  }
  int value = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    value = value * 10 + (ch - '0');
  }
  if (value > 65535) {
    return false;
  }
  port = static_cast<std::uint16_t>(value);
  return true;
}

enum class ArgParse { Ok, Help, Error };

ArgParse parseArgs(int argc, char** argv, std::uint16_t& port, int& idleMs) {
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--help" || arg == "-h") {
      return ArgParse::Help;
    }
    if (arg == "--port") {
      if (index + 1 >= argc || !parsePort(argv[index + 1], port)) {
        return ArgParse::Error;
      }
      ++index;
      continue;
    }
    if (arg == "--idle-ms") {
      if (index + 1 >= argc) {
        return ArgParse::Error;
      }
      int value = 0;
      const std::string text = argv[index + 1];
      if (text.empty() || text.size() > 7) {
        return ArgParse::Error;
      }
      for (char ch : text) {
        if (ch < '0' || ch > '9') {
          return ArgParse::Error;
        }
        value = value * 10 + (ch - '0');
      }
      if (value < 200) {
        return ArgParse::Error;
      }
      idleMs = value;
      ++index;
      continue;
    }
    if (!parsePort(arg, port)) {
      return ArgParse::Error;
    }
  }
  return ArgParse::Ok;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint16_t port = 9000;
  int idleMs = -1;
  const ArgParse parsed = parseArgs(argc, argv, port, idleMs);
  if (parsed == ArgParse::Help) {
    printUsage();
    return 0;
  }
  if (parsed == ArgParse::Error) {
    printUsage();
    return 1;
  }

  minigame::SocketRuntime runtime;
  if (!runtime.ok()) {
    std::cerr << "socket runtime init failed\n";
    return 1;
  }

#ifdef MINIGAME_HAS_MYSQL
  std::unique_ptr<minigame::MysqlAccounts> database;
  if (minigame::loadDotEnv(".env.local")) {
    if (auto settings = minigame::settingsFromEnv("MINIGAME_DB_NAME")) {
      database = std::make_unique<minigame::MysqlAccounts>(*settings);
      if (!database->connected()) {
        minigame::log::warn("database unavailable; continuing without persistence");
        database.reset();
      } else if (!database->applySchema()) {
        minigame::log::warn("database schema failed; continuing without persistence");
        database.reset();
      }
    }
  }
#endif

  minigame::RoomManager rooms;
#ifdef MINIGAME_HAS_MYSQL
  if (database) {
    rooms.setAccounts(database.get());
    minigame::log::info("database persistence enabled");
  }
#endif
  minigame::Server server(port, rooms);
  if (idleMs > 0) {
    server.setIdleTimeout(std::chrono::milliseconds(idleMs));
  }
  if (!server.start()) {
    std::cerr << "failed to start server\n";
    return 1;
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
#ifdef _WIN32
  SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
#endif

  std::atomic<bool> watchDone{false};
  std::thread watcher([&server, &watchDone]() {
    while (!watchDone.load()) {
      if (g_stopRequested != 0) {
        server.stop();
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });

  minigame::log::info("MiniGameServer ready");
  server.run();

  watchDone.store(true);
  if (watcher.joinable()) {
    watcher.join();
  }
  minigame::log::info("MiniGameServer stopped");
  return 0;
}
