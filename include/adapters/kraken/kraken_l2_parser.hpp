#pragma once
#include "adapters/kraken/kraken_book_message.hpp"
#include <optional>
namespace adapters::kraken {
enum class ParseKind { Book, Heartbeat, SubscriptionSuccess, SubscriptionFailure, Ignored, Error };
struct ParseResult {
    ParseKind kind{ParseKind::Error};
    std::optional<KrakenBookMessage> book;
    std::string error;
};
class KrakenL2Parser {
  public:
    ParseResult parse(std::string_view payload, core::ReceiveWallTimestamp wall = {},
                      core::ReceiveMonotonicTimestamp mono = {}) const;
};
// Expand exponent notation without binary floating point, preserving decimal scale.
std::string decimal_token(std::string_view token);
} // namespace adapters::kraken
