#include "adapters/coinbase/reconnect_policy.hpp"
#include <algorithm>
namespace adapters::coinbase {
std::chrono::milliseconds ReconnectPolicy::next_delay() {
    auto result = delay_;
    delay_ = std::min(delay_ * 2, std::chrono::milliseconds{10000});
    return result;
}
} // namespace adapters::coinbase
