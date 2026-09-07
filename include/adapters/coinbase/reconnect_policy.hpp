#pragma once
#include <chrono>
namespace adapters::coinbase {
class ReconnectPolicy {
  public:
    std::chrono::milliseconds next_delay();
    void reset() { delay_ = std::chrono::milliseconds{250}; }

  private:
    std::chrono::milliseconds delay_{250};
};
} // namespace adapters::coinbase
