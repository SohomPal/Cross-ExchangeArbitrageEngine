#include "adapters/kraken/kraken_subscription.hpp"
#include <stdexcept>
namespace adapters::kraken {
void validate_depth(std::size_t d) {
    if (d != 10 && d != 25 && d != 100 && d != 500 && d != 1000)
        throw std::invalid_argument("unsupported Kraken depth");
}
std::string subscription(std::size_t depth) {
    validate_depth(depth);
    return R"({"method":"subscribe","params":{"channel":"book","symbol":["BTC/USD"],"depth":)" +
           std::to_string(depth) + R"(,"snapshot":true},"req_id":1})";
}
} // namespace adapters::kraken
