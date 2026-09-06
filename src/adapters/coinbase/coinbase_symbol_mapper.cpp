#include "adapters/coinbase/coinbase_symbol_mapper.hpp"

#include <stdexcept>
#include <utility>

namespace adapters::coinbase {

CoinbaseSymbolMapper::CoinbaseSymbolMapper()
    : CoinbaseSymbolMapper(Products{{"BTC-USD", {core::Instrument::BTC_USD, 2, 8}}}) {}

CoinbaseSymbolMapper::CoinbaseSymbolMapper(Products products) : products_(std::move(products)) {
    for (const auto& [symbol, config] : products_) {
        if (symbol.empty() || config.price_scale > 18 || config.quantity_scale > 18) {
            throw std::invalid_argument("invalid Coinbase product configuration");
        }
    }
}

std::optional<CoinbaseProductConfig> CoinbaseSymbolMapper::lookup(std::string_view product) const {
    const auto it = products_.find(product);
    if (it == products_.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace adapters::coinbase
