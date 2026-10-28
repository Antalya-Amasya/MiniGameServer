#include "minigame/OutputBuffer.hpp"

namespace minigame {

void OutputBuffer::append(std::string_view bytes) {
  data_.append(bytes.data(), bytes.size());
}

bool OutputBuffer::tryAppend(std::string_view bytes, std::size_t maxBytes) {
  if (data_.size() + bytes.size() > maxBytes) {
    return false;
  }
  data_.append(bytes.data(), bytes.size());
  return true;
}

bool OutputBuffer::empty() const {
  return data_.empty();
}

std::size_t OutputBuffer::size() const {
  return data_.size();
}

OutputBuffer::Flush OutputBuffer::flush(const std::function<int(const char*, int)>& writeSome) {
  while (!data_.empty()) {
    const int written = writeSome(data_.data(), static_cast<int>(data_.size()));
    if (written == -2) {
      return Flush::Again;
    }
    if (written <= 0) {
      return Flush::Error;
    }
    data_.erase(0, static_cast<std::size_t>(written));
  }
  return Flush::Done;
}

}  // namespace minigame
