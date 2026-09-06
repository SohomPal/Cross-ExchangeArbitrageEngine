#pragma once

#include "core/instrument.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace adapters::coinbase {

struct CoinbaseProductConfig {
    core::Instrument instrument;
    std::uint8_t price_scale;
    std::uint8_t quantity_scale;
};

class CoinbaseSymbolMapper {
  public:
    using Products = std::map<std::string, CoinbaseProductConfig, std::less<>>;
    // Synthetic BTC-USD defaults; callers can supply product metadata.
    CoinbaseSymbolMapper();
    explicit CoinbaseSymbolMapper(Products products);
    [[nodiscard]] std::optional<CoinbaseProductConfig> lookup(std::string_view product) const;

  private:
    Products products_;
};

} // namespace adapters::coinbase
