#include "minigame/LineBuffer.hpp"

namespace minigame {

LineBuffer::LineBuffer(std::size_t maxLineLength) : maxLineLength_(maxLineLength) {}

LineBuffer::Status LineBuffer::append(std::string_view bytes) {
  data_.append(bytes.data(), bytes.size());
  std::size_t start = 0;
  while (start < data_.size()) {
    const std::size_t pos = data_.find('\n', start);
    if (pos == std::string::npos) {
      if (data_.size() - start > maxLineLength_) {
        return Status::TooLong;
      }
      return Status::Ok;
    }
    if (pos - start > maxLineLength_) {
      return Status::TooLong;
    }
    start = pos + 1;
  }
  return Status::Ok;
}

bool LineBuffer::next(std::string& line) {
  const std::size_t pos = data_.find('\n');
  if (pos == std::string::npos) {
    return false;
  }
  line.assign(data_, 0, pos);
  data_.erase(0, pos + 1);
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return true;
}

std::size_t LineBuffer::pending() const {
  return data_.size();
}

}  // namespace minigame
