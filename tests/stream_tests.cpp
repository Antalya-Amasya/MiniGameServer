#include "minigame/LineBuffer.hpp"
#include "minigame/OutputBuffer.hpp"
#include "minigame/Protocol.hpp"
#include "minigame/TcpSocket.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

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

void testOneCompleteCommand() {
  minigame::LineBuffer buffer(minigame::proto::kMaxLineLength);
  require(buffer.append("LOGIN alice\n") == minigame::LineBuffer::Status::Ok, "append");
  std::string line;
  require(buffer.next(line), "next");
  require(line == "LOGIN alice", line);
  require(!buffer.next(line), "no more");
}

void testStickyPacket() {
  minigame::LineBuffer buffer(minigame::proto::kMaxLineLength);
  require(buffer.append("LOGIN alice\nCREATE_ROOM\n") == minigame::LineBuffer::Status::Ok, "append");
  std::string line;
  require(buffer.next(line) && line == "LOGIN alice", line);
  require(buffer.next(line) && line == "CREATE_ROOM", line);
  require(!buffer.next(line), "done");
}

void testHalfPacket() {
  minigame::LineBuffer buffer(minigame::proto::kMaxLineLength);
  require(buffer.append("LOG") == minigame::LineBuffer::Status::Ok, "first");
  std::string line;
  require(!buffer.next(line), "incomplete");
  require(buffer.append("IN alice\n") == minigame::LineBuffer::Status::Ok, "second");
  require(buffer.next(line) && line == "LOGIN alice", line);
}

void testFramesAcrossReads() {
  minigame::LineBuffer buffer(minigame::proto::kMaxLineLength);
  require(buffer.append("LOGIN alice\nCRE") == minigame::LineBuffer::Status::Ok, "a");
  std::string line;
  require(buffer.next(line) && line == "LOGIN alice", line);
  require(!buffer.next(line), "half create");
  require(buffer.append("ATE_ROOM\nMOVE 0 0\n") == minigame::LineBuffer::Status::Ok, "b");
  require(buffer.next(line) && line == "CREATE_ROOM", line);
  require(buffer.next(line) && line == "MOVE 0 0", line);
}

void testMoveWithoutNewlineStaysBuffered() {
  minigame::LineBuffer buffer(minigame::proto::kMaxLineLength);
  require(buffer.append("MOVE 0") == minigame::LineBuffer::Status::Ok, "append");
  std::string line;
  require(!buffer.next(line), "no newline");
  require(buffer.pending() == 6, "pending");
}

void testOversizedUnterminatedLine() {
  minigame::LineBuffer buffer(8);
  require(buffer.append("123456789") == minigame::LineBuffer::Status::TooLong, "too long");
}

void testOversizedCompleteLine() {
  minigame::LineBuffer buffer(4);
  require(buffer.append("HELLO\n") == minigame::LineBuffer::Status::TooLong, "line too long");
}

void testOutputBufferPartialWrite() {
  minigame::OutputBuffer buffer;
  buffer.append("HELLO_WORLD");
  std::string written;
  int calls = 0;
  const auto writer = [&written, &calls](const char* data, int length) {
    ++calls;
    if (calls == 2) {
      return -2;
    }
    const int take = length < 3 ? length : 3;
    written.append(data, static_cast<std::size_t>(take));
    return take;
  };
  require(buffer.flush(writer) == minigame::OutputBuffer::Flush::Again, "again");
  require(written == "HEL", written);
  require(!buffer.empty(), "remainder");
  calls = 0;
  const auto drain = [&written](const char* data, int length) {
    const int take = length < 4 ? length : 4;
    written.append(data, static_cast<std::size_t>(take));
    return take;
  };
  require(buffer.flush(drain) == minigame::OutputBuffer::Flush::Done, "done");
  require(written == "HELLO_WORLD", written);
  require(buffer.empty(), "empty");
}

void testOutputBufferWriteError() {
  minigame::OutputBuffer buffer;
  buffer.append("ABC");
  const auto writer = [](const char*, int) { return -1; };
  require(buffer.flush(writer) == minigame::OutputBuffer::Flush::Error, "error");
}

void testSocketCloseIsIdempotent() {
  minigame::SocketRuntime runtime;
  require(runtime.ok(), "runtime");
  minigame::TcpSocket first;
  minigame::TcpSocket second;
  require(minigame::createConnectedPair(first, second), "pair");
  first.close();
  first.close();
  second.close();
  second.close();
  require(!first.valid() && !second.valid(), "closed");
}

}  // namespace

int main() {
  run("one_complete_command", testOneCompleteCommand);
  run("sticky_packet", testStickyPacket);
  run("half_packet", testHalfPacket);
  run("frames_across_reads", testFramesAcrossReads);
  run("move_without_newline", testMoveWithoutNewlineStaysBuffered);
  run("oversized_unterminated_line", testOversizedUnterminatedLine);
  run("oversized_complete_line", testOversizedCompleteLine);
  run("output_buffer_partial_write", testOutputBufferPartialWrite);
  run("output_buffer_write_error", testOutputBufferWriteError);
  run("socket_close_twice", testSocketCloseIsIdempotent);
  std::cout << "STREAM_SUMMARY tests=" << (g_passed + g_failed) << " passed=" << g_passed
            << " failed=" << g_failed << "\n";
  return g_failed == 0 ? 0 : 1;
}
