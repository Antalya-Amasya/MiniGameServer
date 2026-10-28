#include "minigame/TcpSocket.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

class LineLog {
 public:
  void push(std::string line) {
    std::lock_guard<std::mutex> lock(mu_);
    lines_.push_back(std::move(line));
    cv_.notify_all();
  }

  void close() {
    std::lock_guard<std::mutex> lock(mu_);
    closed_ = true;
    cv_.notify_all();
  }

  // 从上次 /wait 停下的位置继续找。
  // 本地回环很快，服务器响应常常比下一条 /wait 先到达，所以不能只看“发出 /wait 之后”的新行。
  // 重复出现的 YOUR_TURN 靠游标往后走，不会再次匹配已经消费过的那一行。
  bool waitNext(const std::string& expected) {
    std::unique_lock<std::mutex> lock(mu_);
    const auto arrived = [this, &expected]() {
      if (closed_) {
        return true;
      }
      for (std::size_t index = cursor_; index < lines_.size(); ++index) {
        if (lines_[index] == expected) {
          return true;
        }
      }
      return false;
    };
    cv_.wait_for(lock, std::chrono::seconds(10), arrived);
    for (std::size_t index = cursor_; index < lines_.size(); ++index) {
      if (lines_[index] == expected) {
        cursor_ = index + 1;
        return true;
      }
    }
    return false;
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<std::string> lines_;
  std::size_t cursor_ = 0;
  bool closed_ = false;
};

void printHelp() {
  std::cerr << "Server commands:\n"
            << "  LOGIN <name>\n"
            << "  CREATE_ROOM\n"
            << "  JOIN_ROOM <id>\n"
            << "  MOVE <row> <col>    row and col are 0..2\n"
            << "  QUIT\n"
            << "Local commands (not sent):\n"
            << "  /wait <exact line>  wait for the next matching server line\n"
            << "  /sleep <ms>\n"
            << "  /help\n"
            << "  /quit\n";
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
  if (value == 0 || value > 65535) {
    return false;
  }
  port = static_cast<std::uint16_t>(value);
  return true;
}

bool parseMillis(const std::string& text, int& value) {
  if (text.empty() || text.size() > 6) {
    return false;
  }
  int parsed = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    parsed = parsed * 10 + (ch - '0');
  }
  value = parsed;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  std::uint16_t port = 9000;
  if (argc >= 2) {
    host = argv[1];
  }
  if (argc >= 3 && !parsePort(argv[2], port)) {
    std::cerr << "Usage: minigame_client [host] [port]\n";
    return 1;
  }
  if (argc > 3) {
    std::cerr << "Usage: minigame_client [host] [port]\n";
    return 1;
  }

  minigame::SocketRuntime runtime;
  if (!runtime.ok()) {
    std::cerr << "socket runtime init failed\n";
    return 1;
  }

  minigame::TcpSocket socket;
  if (!socket.connectTo(host, port)) {
    std::cerr << "connect failed\n";
    return 1;
  }

  LineLog lines;
  std::thread reader([&socket, &lines]() {
    std::string buffer;
    char temp[512];
    while (true) {
      const int received = socket.recvSome(temp, static_cast<int>(sizeof(temp)));
      if (received <= 0) {
        std::cout << "[disconnected]\n" << std::flush;
        lines.close();
        break;
      }
      buffer.append(temp, temp + received);
      for (;;) {
        const std::size_t pos = buffer.find('\n');
        if (pos == std::string::npos) {
          break;
        }
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        lines.push(line);
        std::cout << line << '\n' << std::flush;
      }
    }
  });

  std::cerr << "connected to " << host << ":" << port << "\n";
  printHelp();

  int exitCode = 0;
  std::string input;
  while (std::getline(std::cin, input)) {
    if (!input.empty() && input.back() == '\r') {
      input.pop_back();
    }
    if (input == "/help") {
      printHelp();
      continue;
    }
    if (input.rfind("/wait ", 0) == 0) {
      const std::string expected = input.substr(6);
      if (expected.empty() || !lines.waitNext(expected)) {
        std::cerr << "LOCAL_TIMEOUT " << expected << "\n";
        exitCode = 2;
        break;
      }
      continue;
    }
    if (input.rfind("/sleep ", 0) == 0) {
      int millis = 0;
      if (!parseMillis(input.substr(7), millis)) {
        std::cerr << "usage: /sleep <ms>\n";
        continue;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(millis));
      continue;
    }
    if (input == "/quit") {
      socket.sendAll("QUIT\n");
      break;
    }
    if (!input.empty() && input[0] == '/') {
      std::cerr << "unknown local command\n";
      continue;
    }
    if (!socket.sendAll(input + "\n")) {
      std::cerr << "send failed\n";
      exitCode = 1;
      break;
    }
    if (input == "QUIT") {
      break;
    }
  }

  // 先唤醒读线程，等它离开 recv，再关闭句柄。
  socket.shutdownBoth();
  if (reader.joinable()) {
    reader.join();
  }
  socket.close();
  return exitCode;
}
