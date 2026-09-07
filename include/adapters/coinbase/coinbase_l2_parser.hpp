#pragma once

#include "adapters/coinbase/coinbase_symbol_mapper.hpp"
#include "core/market_event.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace adapters::coinbase {

enum class ParseStatus { Parsed, Ignored, Error };

struct CoinbaseParseResult {
    ParseStatus status;
    std::vector<core::MarketEvent> events;
    std::string error;
    // Owned envelope preserves per-change event_time and unknown metadata.
    std::string raw_message;
    // Envelope sequence is extracted even for ignored non-L2 channels.
    std::optional<std::uint64_t> sequence;
};

class CoinbaseL2Parser {
  public:
    CoinbaseL2Parser() = default;
    explicit CoinbaseL2Parser(CoinbaseSymbolMapper symbols);
    // Errors are atomic across the entire envelope; events is empty on failure.
    [[nodiscard]] CoinbaseParseResult
    parse(std::string_view raw_message, core::ReceiveWallTimestamp receive_wall_time,
          core::ReceiveMonotonicTimestamp receive_monotonic_time) const;

  private:
    CoinbaseSymbolMapper symbols_;
};

} // namespace adapters::coinbase
